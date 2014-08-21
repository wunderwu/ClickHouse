#include <DB/DataStreams/RemoteBlockInputStream.h>
#include <DB/DataStreams/RemoveColumnsBlockInputStream.h>

#include <DB/Storages/StorageDistributed.h>
#include <DB/Storages/VirtualColumnFactory.h>
#include <DB/Storages/Distributed/DistributedBlockOutputStream.h>
#include <DB/Storages/Distributed/DirectoryMonitor.h>
#include <DB/Storages/Distributed/queryToString.h>
#include <DB/Common/escapeForFileName.h>

#include <DB/Interpreters/InterpreterSelectQuery.h>
#include <DB/Interpreters/InterpreterAlterQuery.h>

#include <DB/Core/Field.h>

#include <statdaemons/stdext.h>

namespace DB
{

namespace
{
	template <typename ASTType> void rewriteImpl(ASTType &, const std::string &, const std::string &) = delete;

	/// select query has database and table names as AST pointers
	template <> inline void rewriteImpl<ASTSelectQuery>(ASTSelectQuery & query,
														const std::string & database, const std::string & table)
	{
		query.database = new ASTIdentifier{{}, database, ASTIdentifier::Database};
		query.table = new ASTIdentifier{{}, table, ASTIdentifier::Table};
	}

	/// insert query has database and table names as bare strings
	template <> inline void rewriteImpl<ASTInsertQuery>(ASTInsertQuery & query,
														const std::string & database, const std::string & table)
	{
		query.database = database;
		query.table = table;
		/// make sure query is not INSERT SELECT
		query.select = nullptr;
	}

	/// Создает копию запроса, меняет имена базы данных и таблицы.
	template <typename ASTType>
	inline ASTPtr rewriteQuery(const ASTPtr & query, const std::string & database, const std::string & table)
	{
		/// Создаем копию запроса.
		auto modified_query_ast = query->clone();

		/// Меняем имена таблицы и базы данных
		rewriteImpl(typeid_cast<ASTType &>(*modified_query_ast), database, table);

		return modified_query_ast;
	}
}


StorageDistributed::StorageDistributed(
	const std::string & name_,
	NamesAndTypesListPtr columns_,
	const String & remote_database_,
	const String & remote_table_,
	Cluster & cluster_,
	Context & context_,
	const ASTPtr & sharding_key_,
	const String & data_path_)
	: name(name_), columns(columns_),
	remote_database(remote_database_), remote_table(remote_table_),
	context(context_), cluster(cluster_),
	sharding_key_expr(sharding_key_ ? ExpressionAnalyzer(sharding_key_, context, *columns).getActions(false) : nullptr),
	sharding_key_column_name(sharding_key_ ? sharding_key_->getColumnName() : String{}),
	write_enabled(cluster.getLocalNodesNum() + cluster.pools.size() < 2 || sharding_key_),
	path(data_path_ + escapeForFileName(name) + '/')
{
	createDirectoryMonitors();
}

StoragePtr StorageDistributed::create(
	const std::string & name_,
	NamesAndTypesListPtr columns_,
	const String & remote_database_,
	const String & remote_table_,
	const String & cluster_name,
	Context & context_,
	const ASTPtr & sharding_key_,
	const String & data_path_)
{
	context_.initClusters();

	return (new StorageDistributed{
		name_, columns_, remote_database_, remote_table_,
		context_.getCluster(cluster_name), context_,
		sharding_key_, data_path_
	})->thisPtr();
}


StoragePtr StorageDistributed::create(
	const std::string & name_,
	NamesAndTypesListPtr columns_,
	const String & remote_database_,
	const String & remote_table_,
	SharedPtr<Cluster> & owned_cluster_,
	Context & context_)
{
	auto res = new StorageDistributed{
		name_, columns_, remote_database_,
		remote_table_, *owned_cluster_, context_};

	/// Захватываем владение объектом-кластером.
	res->owned_cluster = owned_cluster_;

	return res->thisPtr();
}

BlockInputStreams StorageDistributed::read(
	const Names & column_names,
	ASTPtr query,
	const Settings & settings,
	QueryProcessingStage::Enum & processed_stage,
	size_t max_block_size,
	unsigned threads)
{
	Settings new_settings = settings;
	new_settings.queue_max_wait_ms = Cluster::saturate(new_settings.queue_max_wait_ms, settings.limits.max_execution_time);

	size_t result_size = cluster.pools.size() + cluster.getLocalNodesNum();

	processed_stage = result_size == 1
		? QueryProcessingStage::Complete
		: QueryProcessingStage::WithMergeableState;

	BlockInputStreams res;
	const auto & modified_query_ast = rewriteQuery<ASTSelectQuery>(
		query, remote_database, remote_table);
	const auto & modified_query = queryToString<ASTSelectQuery>(modified_query_ast);

	/// Цикл по шардам.
	for (auto & conn_pool : cluster.pools)
		res.emplace_back(new RemoteBlockInputStream{
			conn_pool, modified_query, &new_settings,
			external_tables, processed_stage});

	/// Добавляем запросы к локальному ClickHouse.
	if (cluster.getLocalNodesNum() > 0)
	{
		DB::Context new_context = context;
		new_context.setSettings(new_settings);
		for (auto & it : external_tables)
			if (!new_context.tryGetExternalTable(it.first))
				new_context.addExternalTable(it.first, it.second);

		for (size_t i = 0; i < cluster.getLocalNodesNum(); ++i)
		{
			InterpreterSelectQuery interpreter(modified_query_ast, new_context, processed_stage);
			res.push_back(interpreter.execute());
		}
	}

	external_tables.clear();
	return res;
}

BlockOutputStreamPtr StorageDistributed::write(ASTPtr query)
{
	if (!write_enabled)
		throw Exception{
			"Method write is not supported by storage " + getName() +
			" with more than one shard and no sharding key provided",
			ErrorCodes::STORAGE_REQUIRES_PARAMETER
		};

	return new DistributedBlockOutputStream{
		*this,
		rewriteQuery<ASTInsertQuery>(query, remote_database, remote_table)
	};
}

void StorageDistributed::alter(const AlterCommands & params, const String & database_name, const String & table_name, Context & context)
{
	auto lock = lockStructureForAlter();
	params.apply(*columns);
	InterpreterAlterQuery::updateMetadata(database_name, table_name, *columns, context);
}

void StorageDistributed::shutdown()
{
	directory_monitors.clear();
}

NameAndTypePair StorageDistributed::getColumn(const String & column_name) const
{
	if (const auto & type = VirtualColumnFactory::tryGetType(column_name))
		return { column_name, type };

	return getRealColumn(column_name);
}

bool StorageDistributed::hasColumn(const String & column_name) const
{
	return VirtualColumnFactory::hasColumn(column_name) || hasRealColumn(column_name);
}

void StorageDistributed::createDirectoryMonitor(const std::string & name)
{
	directory_monitors.emplace(name, stdext::make_unique<DirectoryMonitor>(*this, name));
}

void StorageDistributed::createDirectoryMonitors()
{
	Poco::File{path}.createDirectory();

	Poco::DirectoryIterator end;
	for (Poco::DirectoryIterator it{path}; it != end; ++it)
		if (it->isDirectory())
			createDirectoryMonitor(it.name());
}

void StorageDistributed::requireDirectoryMonitor(const std::string & name)
{
	if (!directory_monitors.count(name))
		createDirectoryMonitor(name);
}

}
