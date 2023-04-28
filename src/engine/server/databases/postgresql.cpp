#include "base/system.h"
#include "connection.h"

#include <engine/server/databases/connection_pool.h>

#if defined(CONF_POSTGRESQL)
#include <libpq-fe.h>

#include <base/tl/threading.h>
#include <engine/console.h>

#include <atomic>
#include <memory>
#include <vector>

std::atomic_int g_PSQLNumConnections;

bool PSQLAvailable()
{
	return true;
}

class CPSQLConnection : public IDbConnection
{
public:
    explicit CPSQLConnection(CPSQLConfig m_Config);
    ~ CPSQLConnection();
	void Print(IConsole *pConsole, const char *pMode) override;

	bool Connect(char *pError, int ErrorSize) override;
	CPSQLConnection *Copy() override;

private:
    PGconn *m_pConn;
    bool m_NewQuery = false;

    char m_aErrorDetail[128];

    bool ConnectImpl();

	CPSQLConfig m_Config;

	std::atomic_bool m_InUse;
};

CPSQLConnection::CPSQLConnection(CPSQLConfig Config) :
    IDbConnection(Config.m_aPrefix),
    m_Config(Config)
{
    g_PSQLNumConnections += 1;

    m_pConn = nullptr;
    mem_zero(m_aErrorDetail, sizeof(m_aErrorDetail));
}

CPSQLConnection::~CPSQLConnection()
{
    if (m_pConn != nullptr)
    {
        PQfinish(m_pConn);
        m_pConn = nullptr;
    }
	g_PSQLNumConnections -= 1;
}

CPSQLConnection *CPSQLConnection::Copy()
{
	return new CPSQLConnection(m_Config);
}

bool CPSQLConnection::Connect(char *pError, int ErrorSize)
{
	if(m_InUse.exchange(true))
	{
		dbg_assert(0, "Tried connecting while the connection is in use");
	}

	m_NewQuery = true;
	if(ConnectImpl())
	{
		str_copy(pError, m_aErrorDetail, ErrorSize);
		m_InUse.store(false);
		return true;
	}
	return false;
}

bool CPSQLConnection::ConnectImpl()
{
	if(m_pConn)
	{
		// finishing the connection deletes all the statements for us.
		PQfinish(m_pConn);
        m_pConn = nullptr;
	}

	const char OptConnectTimeout[] = "60";

	const char * const Keywords[] = {
		"host",
		"port",
		"user",
		"password",
		"dbname",
		"connect_timeout",
		nullptr
	};

	char aPort[6];
	str_format(aPort, sizeof(aPort), "%d", m_Config.m_Port);

	const char * const Values[] = {
		m_Config.m_aIp,
		aPort,
		m_Config.m_aUser,
		m_Config.m_aPass,
		m_Config.m_aDatabase,
		OptConnectTimeout,
		nullptr
	};

	m_pConn = PQconnectdbParams(Keywords, Values, 0);

	m_pStmt = std::unique_ptr<MYSQL_STMT, CStmtDeleter>(mysql_stmt_init(&m_Mysql));

	// Apparently MYSQL_SET_CHARSET_NAME is not enough
	if(PrepareAndExecuteStatement("SET CHARACTER SET utf8mb4"))
	{
		return true;
	}

	if(m_Config.m_Setup)
	{
		char aCreateDatabase[1024];
		// create database
		str_format(aCreateDatabase, sizeof(aCreateDatabase), "CREATE DATABASE IF NOT EXISTS %s CHARACTER SET utf8mb4", m_Config.m_aDatabase);
		if(PrepareAndExecuteStatement(aCreateDatabase))
		{
			return true;
		}
	}

	// Connect to specific database
	if(mysql_select_db(&m_Mysql, m_Config.m_aDatabase))
	{
		StoreErrorMysql("select_db");
		return true;
	}

	if(m_Config.m_Setup)
	{
		char aCreateRace[1024];
		char aCreateTeamrace[1024];
		char aCreateMaps[1024];
		char aCreateSaves[1024];
		char aCreatePoints[1024];
		FormatCreateRace(aCreateRace, sizeof(aCreateRace), /* Backup */ false);
		FormatCreateTeamrace(aCreateTeamrace, sizeof(aCreateTeamrace), "VARBINARY(16)", /* Backup */ false);
		FormatCreateMaps(aCreateMaps, sizeof(aCreateMaps));
		FormatCreateSaves(aCreateSaves, sizeof(aCreateSaves), /* Backup */ false);
		FormatCreatePoints(aCreatePoints, sizeof(aCreatePoints));

		if(PrepareAndExecuteStatement(aCreateRace) ||
			PrepareAndExecuteStatement(aCreateTeamrace) ||
			PrepareAndExecuteStatement(aCreateMaps) ||
			PrepareAndExecuteStatement(aCreateSaves) ||
			PrepareAndExecuteStatement(aCreatePoints))
		{
			return true;
		}
		m_Config.m_Setup = false;
	}
	dbg_msg("mysql", "connection established");
	return false;
}

void CPSQLConnection::Disconnect()
{
	m_InUse.store(false);
}

std::unique_ptr<IDbConnection> CreatePSQLConnection(CPSQLConfig Config)
{
	return std::make_unique<CPSQLConnection>(Config);
}

#else
bool PSQLAvailable()
{
	return false;
}

std::unique_ptr<IDbConnection> CreatePSQLConnection(CPSQLConfig Config)
{
	return nullptr;
}
#endif
