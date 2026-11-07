//
// Copyright (C) 2013-2026 University of Amsterdam
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public
// License along with this program.  If not, see
// <http://www.gnu.org/licenses/>.
//
#include "databaseinterface.h"
#include "columnutils.h"
#include "columntype.h"
#include "tempfiles.h"
#include <sqlite3.h>
#include "version.h"
#include "dataset.h"
#include "timers.h"
#include <thread>
#include "utils.h"
#include <cassert>
#include <ranges>
#include "log.h"
#include <algorithm>
#include <sstream>
#include <vector>

DatabaseInterface * DatabaseInterface::_singleton	= nullptr;

thread_local 	int			_transactionWriteDepth	= 0,
							_transactionReadDepth	= 0;


//#define SIR_LOG_A_LOT

const std::string DatabaseInterface::_dbConstructionSql =
// The actual definition can be found in "internalDbDefinition.sql"!
#include "internalDbDefinition.h"
;

std::string DatabaseInterface::valuesTableName(int dataSetId) const
{
	return "DataSet_" + std::to_string(dataSetId) + "_Values";
}

std::string DatabaseInterface::filterValuesTableName(int dataSetId) const
{
	return "DataSet_" + std::to_string(dataSetId) + "_FilterValues";
}

const std::string DatabaseInterface::_dbIndexesSql =
#include "createIndexes.h"
;

void DatabaseInterface::upgradeDBFromVersion(Version originalVersion)
{
	transactionWriteBegin();

	try
	{
		_upgradeDBStatements(originalVersion);
		transactionWriteEnd();
	}
	catch(...)
	{
		// A failing ALTER/DDL statement throws while the EXCLUSIVE transaction is still open. Roll it
		// back so the (temporary working-copy) database and the transaction-depth bookkeeping stay
		// consistent, then rethrow the original cause so the load fails loudly instead of continuing
		// on a half-migrated schema.
		Log::log() << "upgradeDBFromVersion(" << originalVersion.asString() << ") failed; rolling back the migration." << std::endl;
		runStatements("ROLLBACK", true); //ignore a failing rollback so it cannot mask the real error
		_transactionWriteDepth = 0;
		throw;
	}
}

//The actual migration statements. Kept separate from upgradeDBFromVersion so that one can wrap them
//in a single transaction with proper rollback-on-failure without indenting this whole block.
void DatabaseInterface::_upgradeDBStatements(Version originalVersion)
{

	if((originalVersion < "0.18.2") && !tableHasColumn("DataSets", "description"))
		runStatements("ALTER TABLE DataSets ADD COLUMN description     TEXT;");

	if((originalVersion < "0.19.0") && tableHasColumn("Columns", "isComputed"))
		runStatements("ALTER TABLE Columns  DROP 	COLUMN isComputed;");		// was removed in 0.18.3
	
	if(originalVersion <= "0.19.0")
	{
		if (!tableHasColumn("Columns", "emptyValuesJson"))
			runStatements("ALTER TABLE Columns  ADD 	COLUMN emptyValuesJson		TEXT;");

		if (!tableHasColumn("Columns", "forceSourceColType"))
			runStatements("ALTER TABLE Columns  ADD 	COLUMN forceSourceColType	INT NULL;");

		if (!tableHasColumn("Columns", "autoSortByValue"))
			runStatements("ALTER TABLE Columns  ADD 	COLUMN autoSortByValue		INT;");

		if (!tableHasColumn("DataSets", "dataFileTimestamp"))
			runStatements("ALTER TABLE DataSets  ADD 	COLUMN dataFileTimestamp	INT;");
	}
	
	if(originalVersion <= "0.19.2")
	{
		if (tableHasColumn("Columns", "forceSourceColType"))
			runStatements("ALTER TABLE Columns  DROP 	COLUMN forceSourceColType;");
	}

	if(originalVersion < "0.19.2" && !tableHasColumn("Filters", "name"))	
		runStatements("ALTER TABLE Filters  ADD COLUMN name		TEXT;");
	
	if(originalVersion <= "0.19.3")
	{
		if(!tableHasColumn("Columns", "dropLevels"))
		{
			runStatements("ALTER TABLE Columns  ADD COLUMN dropLevels		INT;");
			runStatements("UPDATE Columns SET dropLevels = 1;"); //Previously dropLevels was always on, so loading an older jasp-file should have this enabled
		}
		
		runStatements(std::string("UPDATE Filters SET name = '") + DEFAULT_FILTER_NAME + "' WHERE trim(name) = '' OR name IS NULL;"); //Previously the "default filter" didnt have a name, but this is actually not very practical for computeFilter, so lets set it to something on load. Filters will always have a name now.

		if(!tableHasColumn("Columns", "computeFilter"))
			runStatements("ALTER TABLE Columns  ADD COLUMN computeFilter		TEXT DEFAULT \"\";");
		
		if(!tableHasColumn("Labels", "userAdded"))
			runStatements("ALTER TABLE Labels  ADD COLUMN userAdded	INT DEFAULT 0;");
		
		//Create indexes cause they dont exist yet
		runStatements(_dbIndexesSql);
	}

	if(originalVersion <= "0.95.4")
	{
		if(!tableHasColumn("DataSets", "showRSyntax"))
			runStatements("ALTER TABLE DataSets  ADD COLUMN showRSyntax	INT;");
	}

	if(originalVersion < "0.97.0")
	{
		if(!tableHasColumn("Columns", "hasLabels"))
			runStatements(
				"ALTER TABLE Columns  ADD COLUMN hasLabels		INT DEFAULT 0;\n"
				"UPDATE Columns SET hasLabels=1;" //Make sure old columns all "hasLabels" enabled
			);
	}

	//Multi-dataset schema (Workspace table, DataSets.title and Filters.invalidated, together with
	//moving showRSyntax from DataSets into the Workspace) was introduced after upstream/development
	//(which is 0.98.1). Any file created before 0.99 predates it, so bring it up to the current schema.
	if(originalVersion <= "0.99")
	{
		if(!tableExists("Workspace"))
			runStatements("CREATE TABLE Workspace ( "
				"id					INTEGER PRIMARY KEY,"
				"showRSyntax			INT DEFAULT 0	"
			");");

		if(!tableHasColumn("Filters", "invalidated"))
			runStatements("ALTER TABLE Filters  ADD COLUMN invalidated		INT DEFAULT 1;");

		if(!tableHasColumn("DataSets", "title"))
			runStatements("ALTER TABLE DataSets  ADD COLUMN title		TEXT DEFAULT '';");

		if(tableHasColumn("DataSets", "showRSyntax"))
		{
			int showRSyntax = runStatementsId("SELECT showRSyntax FROM DataSets LIMIT 1;");
			//Make sure a Workspace row exists, otherwise the UPDATE below would silently touch nothing
			//and the preserved showRSyntax value would be lost. runStatementsId reports -1 when there
			//are no rows; treat that as the "not stored" default instead of persisting -1.
			runStatements("INSERT OR REPLACE INTO Workspace (id) VALUES (1);");
			runStatements("UPDATE Workspace SET showRSyntax="+std::to_string(showRSyntax > 0 ? showRSyntax : 0)+";");
			runStatements("ALTER TABLE DataSets  DROP COLUMN showRSyntax;");
		}
	}

	//Computed datasets (a whole DataSet generated from R code) store their state on the DataSets
	//table, just like computed columns store it on the Columns table.
	//These are deliberately NOT version-gated (unlike the < 0.99 block above): they are added purely
	//on a tableHasColumn() existence check, so a file saved by an intermediate 0.99 dev/beta build that
	//already went through the Workspace migration but predates these columns still gets them. The
	//checks are no-ops once the columns exist, so running them on every load is harmless.
	if(!tableHasColumn("DataSets", "codeType"))
		runStatements("ALTER TABLE DataSets  ADD COLUMN codeType			TEXT NULL;");
	if(!tableHasColumn("DataSets", "rCode"))
		runStatements("ALTER TABLE DataSets  ADD COLUMN rCode				TEXT NULL;");
	if(!tableHasColumn("DataSets", "invalidated"))
		runStatements("ALTER TABLE DataSets  ADD COLUMN invalidated		INT NULL;");
	if(!tableHasColumn("DataSets", "error"))
		runStatements("ALTER TABLE DataSets  ADD COLUMN error				TEXT NULL;");
	if(!tableHasColumn("DataSets", "defaultInputFilter"))
		runStatements("ALTER TABLE DataSets  ADD COLUMN defaultInputFilter INT NULL;");
	//Not tied to a version: both were added while jaspfiles were already being written with the version number of their release
	if(!tableHasColumn("DataSets", "csvDelimiter"))
		runStatements("ALTER TABLE DataSets ADD COLUMN csvDelimiter INT DEFAULT 0;");
	if(!tableHasColumn("DataSets", "importLocale"))
		runStatements("ALTER TABLE DataSets ADD COLUMN importLocale TEXT DEFAULT \"\";");
	// --- Value-table schema -------------------------------------------------
	if(!tableHasColumn("DataSets", "rowCount"))
		runStatements("ALTER TABLE DataSets ADD COLUMN rowCount INT DEFAULT 0;");

	_migrateToValueSchema(originalVersion);


}

DatabaseInterface::DatabaseInterface(bool createDb, bool inMemory)
	: _inMemory{inMemory}
{
	assert(!_singleton);
	_singleton = this;
	
	if(createDb)	create();
	else			load();
}

DatabaseInterface::~DatabaseInterface()
{
	close();

	_singleton = nullptr;
}


int DatabaseInterface::dataSetInsert(const std::string & dataFilePath, long dataFileTimestamp, const std::string & description, const std::string & databaseJson, const std::string & emptyValuesJson, bool dataSynch, char csvDelimiter, const std::string & importLocale)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetInsert);

	std::function<void(sqlite3_stmt *stmt)> prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_text(stmt, 1, dataFilePath.c_str(),	dataFilePath.length(),		SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt,2, dataFileTimestamp);
		sqlite3_bind_text(stmt, 3, description.c_str(),		description.length(),		SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, databaseJson.c_str(),	databaseJson.length(),		SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, emptyValuesJson.c_str(), emptyValuesJson.length(),	SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt,	6, dataSynch);
		sqlite3_bind_int(stmt,	7, csvDelimiter);
		sqlite3_bind_text(stmt, 8, importLocale.c_str(),	importLocale.length(),		SQLITE_TRANSIENT);
	};

	transactionWriteBegin();
	int id = -1;
	try
	{
		id = runStatementsId(
			"INSERT INTO DataSets (dataFilePath, dataFileTimestamp, description, databaseJson, emptyValuesJson, "
			"dataFileSynch, csvDelimiter, importLocale, rowCount) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0) RETURNING id;", prepare);

		if(id <= 0)
			throw std::runtime_error("dataSetInsert: could not insert a new DataSets row.");

		if(tableExists(valuesTableName(id)))
			throw std::runtime_error("Table '" + valuesTableName(id) + "' already exists!");

		runStatements("CREATE TABLE " + valuesTableName(id) +
			" (columnId INTEGER NOT NULL, rowNumber INTEGER NOT NULL, value NUM, "
			"  PRIMARY KEY (columnId, rowNumber)) WITHOUT ROWID;");

		runStatements("CREATE TABLE " + filterValuesTableName(id) +
			" (filterId INTEGER NOT NULL, rowNumber INTEGER NOT NULL, value INT NOT NULL DEFAULT 1, "
			"  PRIMARY KEY (filterId, rowNumber)) WITHOUT ROWID;");
	}
	catch(const std::exception &)
	{
		if(id > 0)
		{
			try { dataSetDelete(id); } catch(...) {}
		}
		transactionWriteEnd();
		throw;
	}
	transactionWriteEnd();

	return id;
}

void DatabaseInterface::dataSetUpdate(int dataSetId,	const std::string & title, const std::string & dataFilePath, long dataFileTimestamp, const std::string & description, const std::string & databaseJson, const std::string & emptyValuesJson, bool dataSynch, char csvDelimiter, const std::string & importLocale)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetUpdate);
	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_text(stmt, 1, dataFilePath.c_str(),	dataFilePath.length(),		SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 2, title.c_str(),			title.length(),				SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 3, dataFileTimestamp);
		sqlite3_bind_text(stmt, 4, description.c_str(),		description.length(),		SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, databaseJson.c_str(),	databaseJson.length(),		SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 6, emptyValuesJson.c_str(), emptyValuesJson.length(),	SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt,	7, dataSynch);
		sqlite3_bind_int(stmt,	8, csvDelimiter);
		sqlite3_bind_text(stmt, 9, importLocale.c_str(),	importLocale.length(),		SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 10, dataSetId);
	};

	//Log::log() << "UPDATE DataSet " << dataSetId << " with Empty Values: " << emptyValuesJson << std::endl;

	runStatements("UPDATE DataSets SET dataFilePath=?, title=?, dataFileTimestamp=?, description=?, databaseJson=?, emptyValuesJson=?, dataFileSynch=?, csvDelimiter=?, importLocale=?, revision=revision+1 WHERE id = ?;", prepare);
}

void DatabaseInterface::dataSetLoad(int dataSetId, std::string & title, std::string & dataFilePath, long & dataFileTimestamp, std::string & description, std::string & databaseJson, std::string & emptyValuesJson, int & revision, bool & dataSynch, char & csvDelimiter, std::string & importLocale)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetLoad);

	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt, 1, dataSetId);
	};

	std::function<void(size_t row, sqlite3_stmt *stmt)> processRow = [&](size_t row, sqlite3_stmt *stmt)
	{
		int colCount = sqlite3_column_count(stmt);

		assert(colCount == 10);

		dataFilePath		= _wrap_sqlite3_column_text(stmt, 0);
		title				= _wrap_sqlite3_column_text(stmt, 1);
		dataFileTimestamp	= sqlite3_column_int64(stmt, 2);
		description			= _wrap_sqlite3_column_text(stmt, 3);
		databaseJson		= _wrap_sqlite3_column_text(stmt, 4);
		emptyValuesJson		= _wrap_sqlite3_column_text(stmt, 5);
		revision			= sqlite3_column_int(	stmt, 6);
		dataSynch			= sqlite3_column_int(	stmt, 7);
		csvDelimiter		= static_cast<char>(sqlite3_column_int(stmt, 8));
		importLocale		= _wrap_sqlite3_column_text(stmt, 9);

		//Log::log() << "Output loadDataset(dataSetId="<<dataSetId<<") had (dataFilePath='"<<dataFilePath<<"', databaseJson='"<<databaseJson<<"', emptyValuesJson='"<<emptyValuesJson<<"')" << std::endl;
	};

	runStatements("SELECT dataFilePath, title, dataFileTimestamp, description, databaseJson, emptyValuesJson, revision, dataFileSynch, csvDelimiter, importLocale FROM DataSets WHERE id = ?;", prepare, processRow);
}

void DatabaseInterface::dataSetSetComputedInfo(int dataSetId, bool invalidated, computedColumnType codeType, const std::string & rCode, const std::string & error, int defaultInputFilter)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetSetComputedInfo);

	runStatements("UPDATE DataSets SET invalidated=?, codeType=?, rCode=?, error=?, defaultInputFilter=? WHERE id=?;", [&](sqlite3_stmt * stmt)
	{
		std::string codeT = computedColumnTypeToString(codeType);

		sqlite3_bind_int(stmt,  1, int(invalidated));
		sqlite3_bind_text(stmt, 2, codeT.c_str(),		codeT.length(),		SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, rCode.c_str(),		rCode.length(),		SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, error.c_str(),		error.length(),		SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt,  5, defaultInputFilter);
		sqlite3_bind_int(stmt,  6, dataSetId);
	});
}

void DatabaseInterface::dataSetGetComputedInfo(int dataSetId, bool & invalidated, computedColumnType & codeType, std::string & rCode, std::string & error, int & defaultInputFilter)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetGetComputedInfo);

	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt, 1, dataSetId);
	};

	std::function<void(size_t row, sqlite3_stmt *stmt)> processRow = [&](size_t row, sqlite3_stmt *stmt)
	{
		int colCount = sqlite3_column_count(stmt);

		assert(colCount == 5);

					invalidated			= sqlite3_column_int(		stmt,	0);
		std::string codeTypeStr			= _wrap_sqlite3_column_text(stmt,	1);
					rCode				= _wrap_sqlite3_column_text(stmt,	2);
					error				= _wrap_sqlite3_column_text(stmt,	3);
					defaultInputFilter	= sqlite3_column_int(		stmt,	4);

		codeType = computedColumnType::notComputed;
		if (!codeTypeStr.empty())
		{
			try { codeType = computedColumnTypeFromString(codeTypeStr); }
			catch(...) {}
		}
	};

	runStatements("SELECT invalidated, codeType, rCode, error, defaultInputFilter FROM DataSets WHERE id = ?;", prepare, processRow);
}

int DatabaseInterface::dataSetColCount(int dataSetId)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetColCount);
	return singleton()->runStatementsId("SELECT COUNT(id) FROM Columns WHERE dataSet="+std::to_string(dataSetId));
}

int DatabaseInterface::dataSetRowCount(int dataSetId)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetRowCount);
	return singleton()->runStatementsId("SELECT rowCount FROM DataSets WHERE id="+std::to_string(dataSetId));
}

void DatabaseInterface::dataSetSetRowCount(int dataSetId, size_t rowCount)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetSetRowCount);
	transactionWriteBegin();

	const int64_t curCount = dataSetRowCount(dataSetId);

	runStatements("UPDATE DataSets SET rowCount=? WHERE id=?;", [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int64(stmt, 1, (int64_t)rowCount);
		sqlite3_bind_int(stmt,   2, dataSetId);
	});

	if((int64_t)rowCount < curCount)
	{
		// Removing rows: prune value tables (rare; acceptable scan by rowNumber).
		runStatements("DELETE FROM " + valuesTableName(dataSetId) +
					  " WHERE rowNumber > " + std::to_string(rowCount) + ";");
		runStatements("DELETE FROM " + filterValuesTableName(dataSetId) +
					  " WHERE rowNumber > " + std::to_string(rowCount) + ";");
	}

	transactionWriteEnd();
}

void DatabaseInterface::filterClear(int filterId)
{
	JASPTIMER_SCOPE(DatabaseInterface::filterClear);
	const int ds = filterGetDataSetId(filterId);
	if(ds != -1)
		runStatements("DELETE FROM " + filterValuesTableName(ds) + " WHERE filterId=?;",
					[&](sqlite3_stmt *stmt){ sqlite3_bind_int(stmt, 1, filterId); });
}

void DatabaseInterface::filterDelete(int filterId)
{
	JASPTIMER_SCOPE(DatabaseInterface::filterDelete);
	transactionWriteBegin();

	const int ds = filterGetDataSetId(filterId);

	if(ds != -1)
		runStatements("DELETE FROM " + filterValuesTableName(ds) + " WHERE filterId=?;",
					[&](sqlite3_stmt *stmt){ sqlite3_bind_int(stmt, 1, filterId); });

	runStatements("DELETE FROM Filters WHERE id = " + std::to_string(filterId) + ";");

	transactionWriteEnd();
}


int DatabaseInterface::filterInsert(int dataSetId, const std::string & rFilter, const std::string & generatedFilter, const std::string & constructorJson, const std::string & constructorR, const std::string & name)
{
	JASPTIMER_SCOPE(DatabaseInterface::filterInsertDataSet);
	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int( stmt, 1, dataSetId);
		sqlite3_bind_text(stmt, 2, rFilter.c_str(),				rFilter.length(),			SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, generatedFilter.c_str(),		generatedFilter.length(),	SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, constructorJson.c_str(),		constructorJson.length(),	SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, constructorR.c_str(),		constructorR.length(),		SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 6, name.c_str(),				name.length(),				SQLITE_TRANSIENT);
	};

	transactionWriteBegin();
	
	const int id = runStatementsId(
				"INSERT INTO Filters (dataSet, rFilter, generatedFilter, constructorJson, constructorR, name) "
				"VALUES (?, ?, ?, ?, ?, ?) RETURNING rowid;", prepare);
	
	transactionWriteEnd();

	return id;
}


//This one only works when there is but 1 filter per dataset, this might change later
int DatabaseInterface::filterGetId(	int dataSetId)
{
	JASPTIMER_SCOPE(DatabaseInterface::filterGetId);
	int filterId = -1;

	runStatements("SELECT id FROM Filters WHERE dataSet = ? AND name = ''",
		[&](sqlite3_stmt *stmt)				{ sqlite3_bind_int(stmt,	1, dataSetId); },
		[&](size_t row, sqlite3_stmt *stmt)	{ filterId = sqlite3_column_int(stmt, 0); }
	);

	return filterId;
}

int DatabaseInterface::filterGetId(int dataSetId, const std::string &name)
{
	JASPTIMER_SCOPE(DatabaseInterface::filterGetId);
	int filterId = -1;

	runStatements("SELECT id FROM Filters WHERE name = ? and dataSet = ?",
		[&](sqlite3_stmt *stmt)				{ 
			sqlite3_bind_text(	stmt, 1, name.c_str(), name.length(), SQLITE_TRANSIENT);	
			sqlite3_bind_int(	stmt, 2, dataSetId);
		},
		[&](size_t row, sqlite3_stmt *stmt)	{ filterId = sqlite3_column_int(stmt, 0);										}
	);

	return filterId;
}

int DatabaseInterface::filterGetDataSetId(int filterIndex)
{
	JASPTIMER_SCOPE(DatabaseInterface::filterGetDataSetId);
	return runStatementsId("SELECT dataSet from Filters WHERE id=" + std::to_string(filterIndex));
}

std::string DatabaseInterface::filterGetName(int filterIndex)
{
	JASPTIMER_SCOPE(DatabaseInterface::filterGetName);
	std::string errorMsg;

	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt, 1, filterIndex);
	};

	std::function<void(size_t row, sqlite3_stmt *stmt)> processRow = [&](size_t row, sqlite3_stmt *stmt)
	{
		int colCount = sqlite3_column_count(stmt);

		assert(colCount == 1);
		errorMsg		= _wrap_sqlite3_column_text(stmt, 0);
	};

	runStatements("SELECT name FROM Filters WHERE id = ?;", prepare, processRow);

	return errorMsg;
}

bool DatabaseInterface::filterSelect(int filterId, boolvec & bools)
{
	JASPTIMER_SCOPE(DatabaseInterface::filterSelect);
	bool changed = false;

	transactionReadBegin();

	const int ds = filterGetDataSetId(filterId);

	if(ds != -1)
	{
		const size_t rows = dataSetRowCount(ds);

		changed = changed || rows != bools.size();

		bools.resize(rows);
		std::fill(bools.begin(), bools.end(), true);

		runStatements("SELECT rowNumber, value FROM " + filterValuesTableName(ds) +
					  " WHERE filterId=? AND rowNumber<=? ORDER BY rowNumber",
			[&](sqlite3_stmt *stmt) {
				sqlite3_bind_int(stmt,   1, filterId);
				sqlite3_bind_int64(stmt, 2, (int64_t)rows);
			},
			[&](size_t, sqlite3_stmt *stmt) {
				const int64_t rn  = sqlite3_column_int64(stmt, 0);
				const bool    val = sqlite3_column_int(stmt, 1) != 0;
				const size_t  idx = (size_t)(rn - 1);
				if(idx < bools.size())
				{
					if(bools[idx] != val) changed = true;
					bools[idx] = val;
				}
			});
	}

	transactionReadEnd();

	return changed;
}

void DatabaseInterface::filterUpdate(int filterIndex, const std::string & rFilter, const std::string & generatedFilter, const std::string & constructorJson, const std::string & constructorR, const std::string & name, bool invalidated)
{
	JASPTIMER_SCOPE(DatabaseInterface::filterUpdate);
	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_text(stmt, 1, rFilter.c_str(),			rFilter.length(),			SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 2, generatedFilter.c_str(),	generatedFilter.length(),	SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, constructorJson.c_str(),	constructorJson.length(),	SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, constructorR.c_str(),	constructorR.length(),		SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, name.c_str(),			name.length(),				SQLITE_TRANSIENT);
		sqlite3_bind_int (stmt,	6, invalidated);
		sqlite3_bind_int (stmt,	7, filterIndex);
	};

	runStatements("UPDATE Filters SET rFilter=?, generatedFilter=?, constructorJson=?, constructorR=?, name=?, invalidated=? WHERE id = ?;", prepare);
}

void DatabaseInterface::filterLoad(int filterIndex, std::string & rFilter, std::string & generatedFilter, std::string & constructorJson, std::string & constructorR, int & revision, std::string & name, bool & invalidated)
{
	JASPTIMER_SCOPE(DatabaseInterface::filterLoad);
	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt, 1, filterIndex);
	};

	std::function<void(size_t row, sqlite3_stmt *stmt)> processRow = [&](size_t row, sqlite3_stmt *stmt)
	{
		int colCount = sqlite3_column_count(stmt);

		assert(colCount == 7);
		rFilter			= _wrap_sqlite3_column_text(stmt, 0);
		generatedFilter	= _wrap_sqlite3_column_text(stmt, 1);
		constructorJson	= _wrap_sqlite3_column_text(stmt, 2);
		constructorR	= _wrap_sqlite3_column_text(stmt, 3);
		revision		= sqlite3_column_int(		stmt, 4);
		name			= _wrap_sqlite3_column_text(stmt, 5);
		invalidated		= sqlite3_column_int(		stmt, 6);
	};

	runStatements("SELECT rFilter, generatedFilter, constructorJson, constructorR, revision, name, invalidated FROM Filters WHERE id = ?;", prepare, processRow);
}

std::string DatabaseInterface::filterLoadErrorMsg(int filterIndex)
{
	JASPTIMER_SCOPE(DatabaseInterface::filterLoadErrorMsg);
	std::string errorMsg;
	
	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt, 1, filterIndex);
	};

	std::function<void(size_t row, sqlite3_stmt *stmt)> processRow = [&](size_t row, sqlite3_stmt *stmt)
	{
		int colCount = sqlite3_column_count(stmt);

		assert(colCount == 1);
		errorMsg		= _wrap_sqlite3_column_text(stmt, 0);
	};

	runStatements("SELECT errorMsg FROM Filters WHERE id = ?;", prepare, processRow);
	
	return errorMsg;
}

void DatabaseInterface::filterUpdateErrorMsg(int filterIndex, const std::string & errorMsg)
{
	JASPTIMER_SCOPE(DatabaseInterface::filterUpdateErrorMsg);
	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_text(stmt, 1, errorMsg.c_str(),	errorMsg.length(),			SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt,	2, filterIndex);
	};

	runStatements("UPDATE Filters SET errorMsg=? WHERE id = ?;", prepare);
}

int DatabaseInterface::filterIncRevision(int filterIndex)
{
	JASPTIMER_SCOPE(DatabaseInterface::filterIncRevision);
	transactionWriteBegin();

	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt, 1, filterIndex);
	};

				runStatements(	"UPDATE Filters SET revision=revision+1	WHERE id=?;", prepare);
	int rev =	runStatementsId("SELECT revision FROM Filters			WHERE id=?;", prepare);

	transactionWriteEnd();

	return rev;
}

int DatabaseInterface::filterGetRevision(int filterIndex)
{
	JASPTIMER_SCOPE(DatabaseInterface::filterGetRevision);
	return runStatementsId("SELECT revision FROM Filters	WHERE id=?;", [&](sqlite3_stmt *stmt) { sqlite3_bind_int(stmt, 1, filterIndex); });
}

void DatabaseInterface::filterWrite(int filterId, const std::vector<bool> & values)
{
	JASPTIMER_SCOPE(DatabaseInterface::filterWrite);

	transactionWriteBegin();

	const int ds = filterGetDataSetId(filterId);

	if(ds != -1 && !values.empty())
	{
		runStatements("DELETE FROM " + filterValuesTableName(ds) + " WHERE filterId=?;",
			[&](sqlite3_stmt *stmt){ sqlite3_bind_int(stmt, 1, filterId); });

		// Only store non-default (false) values. Batch the INSERTs to keep the statement small.
		std::vector<size_t> zeroRows;
		zeroRows.reserve(values.size());
		for(size_t i = 0; i < values.size(); i++)
			if(!values[i]) zeroRows.push_back(i);

		const size_t BATCH = 500;
		for(size_t offset = 0; offset < zeroRows.size(); offset += BATCH)
		{
			const size_t end = std::min(offset + BATCH, zeroRows.size());
			std::stringstream ss;
			ss << "INSERT INTO " << filterValuesTableName(ds)
			   << " (filterId, rowNumber, value) VALUES ";
			for(size_t k = offset; k < end; k++)
			{
				if(k > offset) ss << ",";
				ss << "(?," << (zeroRows[k] + 1) << ",0)";
			}
			const size_t count = end - offset;
			runStatements(ss.str(), [&](sqlite3_stmt *stmt) {
				for(size_t k = 0; k < count; k++)
					sqlite3_bind_int(stmt, (int)(k + 1), filterId);
			});
		}
	}

	filterIncRevision(filterId);

	transactionWriteEnd();
}

int DatabaseInterface::columnInsert(int dataSetId, int index, const std::string & name, columnType colType, bool alterTable)
{
	intvec ids = columnsInsert(dataSetId, 1, index, name, colType, alterTable);
	return ids.front();
}

intvec DatabaseInterface::columnsInsert(int dataSetId, int count, int index, const std::string & name, columnType colType, bool alterTable)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnInsert);
	transactionWriteBegin();
	
	if(index == -1)	index = columnLastFreeIndex(dataSetId);
	else			columnIndexIncrements(dataSetId, index, count);

#ifdef SIR_LOG_A_LOT
	Log::log() << "columnIndex for insert: " << index << " and dataSet: " << dataSetId << std::endl;
#endif
	
	intvec ids;
	int curCol = 0;
	
	bindParametersType _bindParams =  [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_int(stmt,	1, dataSetId);
		sqlite3_bind_text(stmt, 2, name.c_str(), name.length(), SQLITE_TRANSIENT);

		std::string colT = columnTypeToString(colType);
		sqlite3_bind_text(stmt, 3, colT.c_str(), colT.length(), SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt,	4, index+curCol++);
	};

	std::function<void(size_t,size_t, sqlite3_stmt*)> processRow = [&](size_t row, size_t rep, sqlite3_stmt * stmt)
	{
		assert(sqlite3_column_count(stmt) == 1);

		ids.push_back(sqlite3_column_int(stmt, 0));
	};
	
	_runStatementsRepeatedly("INSERT INTO Columns (dataSet, name, columnType, colIdx, analysisId) VALUES (?, ?, ?, ?, -1) RETURNING id;", [&](bindParametersType ** bindParams, size_t)
		{
			(*bindParams) = &_bindParams;
			
			return curCol < count;
		}, & processRow);
	

	// Value-table architecture: no ALTER TABLE needed. Values live in DataSet_<id>_Values.
	// The labels will be added separately later. so we disable bellow,
	
	// if(alterTable) //If not then via dataSetCreateTable
	// {
	// 	std::stringstream statement;
		
	// 	for(int id : ids)
	// 		statement	<< "ALTER TABLE " << dataSetName(dataSetId)
	// 					<< " ADD  "		<< columnBaseName(id)
	// 					<< " NUM  NULL;\n";
		
	// 	runStatements(statement.str());
	// }
	
	transactionWriteEnd();
	return ids;
}


void DatabaseInterface::dataSetCreateTable(DataSet * dataSet)
{
	runStatements("DELETE FROM " + valuesTableName(dataSet->id()) + ";");
	runStatements("DELETE FROM " + filterValuesTableName(dataSet->id()) + ";");
	runStatements("UPDATE DataSets SET rowCount=0 WHERE id=?;",
			[&](sqlite3_stmt *stmt){ sqlite3_bind_int(stmt, 1, dataSet->id()); });
}

int DatabaseInterface::columnGetDataSetId(int columnId)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnGetDataSetId);
	return runStatementsId("SELECT dataSet from Columns WHERE id=" + std::to_string(columnId));
}

int	DatabaseInterface::columnLastFreeIndex(int dataSetId)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnLastFreeIndex);
	return 1 + runStatementsId("SELECT MAX(colIdx) from Columns WHERE dataSet=" + std::to_string(dataSetId) + ";");
}

void DatabaseInterface::columnIndexIncrements(int dataSetId, int index, int count)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnIndexIncrements);
	if(columnIdForIndex(dataSetId, index) != -1)
		runStatements("UPDATE Columns SET colIdx=colIdx+"+std::to_string(count)+ " WHERE dataSet=" + std::to_string(dataSetId) + " AND colIdx >= " + std::to_string(index) +";");
//Actually the following else is not necessary
//	else
//		throw std::runtime_error("columnIndexIncrements has a problem: index " + std::to_string(index) + " in dataSet " + std::to_string(dataSetId) + " already exists!");
}

void DatabaseInterface::columnIndexDecrements(int dataSetId, int index)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnIndexDecrements);
	if(columnIdForIndex(dataSetId, index) == -1)
		runStatements("UPDATE Columns SET colIdx=colIdx-1 WHERE dataSet=" + std::to_string(dataSetId) + " AND colIdx > " + std::to_string(index) +";");
}

int DatabaseInterface::columnIdForIndex(int dataSetId, int index)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnIdForIndex);
	return runStatementsId("SELECT id FROM Columns WHERE dataSet=? AND colIdx=?", [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_int(stmt,	1, dataSetId);
		sqlite3_bind_int(stmt,	2, index);
	});
}

int DatabaseInterface::columnIndexForId(int columnId)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnIndexForId);
	return runStatementsId("SELECT colIdx FROM Columns WHERE id=?", [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_int(stmt,	1, columnId);
	});
}

void DatabaseInterface::dataSetBatchedValuesUpdate(DataSet * data, Columns columns, std::function<void(float)> progressCallback)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetBatchedValuesUpdate);

	if(!data) return;
	if(columns.empty()) columns = data->columns();

	transactionWriteBegin();

	const int ds = data->id();
	const std::string vt = valuesTableName(ds);

	// 1) Delete existing values for these columns in one shot.
	if(!columns.empty())
	{
		std::stringstream del;
		del << "DELETE FROM " << vt << " WHERE columnId IN (";
		bool first = true;
		for(Column * col : columns)
		{
			if(!first) del << ",";
			del << col->id();
			first = false;
		}
		del << ");";
		runStatements(del.str());
	}

	// 2) Per-column bulk insert (skipping missing values).
	const size_t totalCols = columns.size();
	size_t doneCols = 0;
	for(Column * col : columns)
	{
		const bool hasLabels	= col->hasLabels();
		const size_t rowCount	= col->rowCount();

		size_t nextRow		= 0;
		size_t rowOutside	= 0;
		bindParametersType bind = [&](sqlite3_stmt *stmt)
		{
			sqlite3_bind_int(stmt,   1, col->id());
			sqlite3_bind_int64(stmt, 2, (int64_t)rowOutside + 1);
			if(hasLabels)
				sqlite3_bind_int(stmt, 3, col->ints()[rowOutside]);
			else
			{
				const std::string & s = col->strs()[rowOutside];
				if(s.empty())
					_doubleTroubleBinder(stmt, 3, col->dbls()[rowOutside]);
				else
					sqlite3_bind_text(stmt, 3, s.c_str(), (int)s.size(), SQLITE_TRANSIENT);
			}
		};

		auto isMissing = [&](size_t r) -> bool {
			if(hasLabels) return col->ints()[r] == EmptyValues::missingValueInteger;
			return std::isnan(col->dbls()[r]) && col->strs()[r].empty();
		};

		_runStatementsRepeatedly(
			"INSERT INTO " + vt + " (columnId, rowNumber, value) VALUES (?, ?, ?)",
			[&](bindParametersType ** bp, size_t)
			{
				while(nextRow < rowCount && isMissing(nextRow)) nextRow++;
				if(nextRow >= rowCount) return false;
				rowOutside = nextRow;
				nextRow++;
				*bp = &bind;
				return true;
			});

		doneCols++;
		progressCallback(0.9f * float(doneCols) / float(totalCols));
	}

	// 3) Default filter values (only store non-default = false).
	if(data->defaultFilter() && data->defaultFilter()->id() > 0)
	{
		const int fid = data->defaultFilter()->id();
		runStatements("DELETE FROM " + filterValuesTableName(ds) + " WHERE filterId=?;",
			[&](sqlite3_stmt *stmt){ sqlite3_bind_int(stmt, 1, fid); });

		const auto & filtered = data->defaultFilter()->filtered();
		const size_t filterRows = std::min<size_t>(filtered.size(), (size_t)data->rowCount());

		std::vector<size_t> zeroRows;
		zeroRows.reserve(filterRows);
		for(size_t i = 0; i < filterRows; i++)
			if(!filtered[i]) zeroRows.push_back(i);

		const size_t BATCH = 500;
		for(size_t offset = 0; offset < zeroRows.size(); offset += BATCH)
		{
			const size_t end = std::min(offset + BATCH, zeroRows.size());
			std::stringstream ss;
			ss << "INSERT INTO " << filterValuesTableName(ds) << " (filterId, rowNumber, value) VALUES ";
			for(size_t k = offset; k < end; k++)
			{
				if(k > offset) ss << ",";
				ss << "(?," << (zeroRows[k] + 1) << ",0)";
			}
			const size_t count = end - offset;
			runStatements(ss.str(), [&](sqlite3_stmt *stmt) {
				for(size_t k = 0; k < count; k++)
					sqlite3_bind_int(stmt, (int)(k + 1), fid);
			});
		}
	}

	// 4) Record the new row count on DataSets.
	runStatements("UPDATE DataSets SET rowCount=? WHERE id=?;", [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int64(stmt, 1, (int64_t)data->rowCount());
		sqlite3_bind_int(stmt,   2, ds);
	});

	progressCallback(1.0f);

	transactionWriteEnd();
}

void DatabaseInterface::dataSetBatchedValuesUpdate(DataSet * data, std::function<void(float)> progressCallback)
{
	dataSetBatchedValuesUpdate(data, data ? data->columns() : Columns(), progressCallback);
}

void DatabaseInterface::dataSetBatchedValuesLoad(DataSet *data, std::function<void(float)> progressCallback)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetBatchedValuesLoad);

	if(!data) return;

	if(data->defaultFilter()->id() == -1)
		data->defaultFilter()->setId(dataSetGetDefaultFilter(data->id()));

	if(data->columns().size() == 0 && data->defaultFilter()->id() == -1)
		return;

	transactionReadBegin();

	const int ds = data->id();
	const std::string vt  = valuesTableName(ds);
	const std::string fvt = filterValuesTableName(ds);
	const size_t rows = data->rowCount();

	// Resize columns
	for(Column * col : data->columns())
		col->setRowCount(rows);

	// Default filter
	if(data->defaultFilter()->id() > 0)
	{
		data->defaultFilter()->setRowCount(rows);

		for(size_t i = 0; i < rows; i++)
			data->defaultFilter()->setFilterValueNoDB(i, true);

		runStatements(
			"SELECT rowNumber, value FROM " + fvt + " WHERE filterId=? AND rowNumber<=? ORDER BY rowNumber",
			[&](sqlite3_stmt *stmt) {
				sqlite3_bind_int(stmt,   1, data->defaultFilter()->id());
				sqlite3_bind_int64(stmt, 2, (int64_t)rows);
			},
			[&](size_t, sqlite3_stmt *stmt) {
				const int64_t rn = sqlite3_column_int64(stmt, 0);
				if((size_t)(rn - 1) < rows)
					data->defaultFilter()->setFilterValueNoDB((size_t)(rn - 1), sqlite3_column_int(stmt, 1) != 0);
			});
	}

	// Parallel column loading
	std::mutex progressMutex;
	size_t progressDone = 0;
	const size_t totalCols = data->columns().size();

	auto loadBatchOfColumns = [this, vt, rows, &progressMutex, &progressDone, totalCols, &progressCallback](Columns group)
	{
		for(Column * col : group)
		{
			runStatements(
				"SELECT rowNumber, value FROM " + vt + " WHERE columnId=? AND rowNumber<=? ORDER BY rowNumber",
				[&](sqlite3_stmt *stmt) {
					sqlite3_bind_int(stmt,   1, col->id());
					sqlite3_bind_int64(stmt, 2, (int64_t)rows);
				},
				[&](size_t, sqlite3_stmt *stmt) {
					const int64_t rn = sqlite3_column_int64(stmt, 0);
					const size_t  i  = (size_t)(rn - 1);
					if(i >= (size_t)col->rowCount()) return;
					if(col->hasLabels())
					{
						int v = (sqlite3_column_type(stmt, 1) == SQLITE_NULL)
							? EmptyValues::missingValueInteger
							: sqlite3_column_int(stmt, 1);
						col->setValue(i, v, false);
					}
					else
					{
						std::string s;
						double d = _doubleTroubleReader(stmt, 1, &s);
						col->_strs[i] = s;
						col->_dbls[i] = d;
					}
				});

			std::lock_guard<std::mutex> lg(progressMutex);
			progressDone++;
			progressCallback(float(progressDone) / float(totalCols));
		}
	};

	const size_t threadCount = std::max<size_t>(1, std::thread::hardware_concurrency());
	const size_t groupSize	= totalCols / threadCount;
	const size_t groupSize0	= totalCols - groupSize * (threadCount - 1);

	std::vector<std::thread> threads;
	size_t curCol = 0, nextEnd = groupSize0;

	for(size_t g = 0; g < threadCount; g++)
	{
		Columns cols;
		for(; curCol < nextEnd; curCol++)
			cols.push_back(data->column(curCol));

		threads.push_back(std::thread([this, cols, &loadBatchOfColumns]()
		{
			preloadInterfaceForThread();
			loadBatchOfColumns(cols);
		}));

		nextEnd += groupSize;
	}

	for(std::thread & t : threads)
		t.join();

	transactionReadEnd();
}

//Shouldve probably been more generic here
void DatabaseInterface::dataSetBatchedLabelsLoad(DataSet *data, std::function<void (float)> progressCallback)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetBatchedLabelsLoad);

	if(data->columns().size() == 0)
		return;

	transactionReadBegin();
	
	//Set up some functions and such for concurrent loading:
	std::mutex	progressMutex;
	size_t 		totalCols	= data->columnCount(), // for progressbar
				progressCol = 0,
				lastCol		= 0;
	
	
	std::function<void(float)> localProgressBar = [&progressMutex, &progressCol, &totalCols, &lastCol, &progressCallback](int rows)
	{
		progressMutex.lock();
		progressCol += rows;
		
		const size_t rowPercent = std::max(1, int(totalCols) / 100);
		
		if(progressCol - lastCol > rowPercent || progressCol >= totalCols - 1)
		{
			progressCallback(float(progressCol) / float(totalCols));
			lastCol = progressCol;
		}
		progressMutex.unlock();
	};
	
	auto loadBatchOfColumns = [this, data, &localProgressBar](Columns group, size_t groupNum)
	{
		labelsLoad(group);
		localProgressBar(groupNum);
	};
	
	//Ok, split up columns into some groups so we can use multiple threads
	Columns columnsWithLabels;
	std::copy_if(data->columns().begin(), data->columns().end(), std::back_inserter(columnsWithLabels), 
				 [](Column * col){return col->hasLabels();});
	
	if(columnsWithLabels.size() == 0)
	{
		transactionReadEnd();
		return;
	}
		
	
	size_t	groupCount	= std::max((unsigned int)1, std::thread::hardware_concurrency()),
			groupSize	= columnsWithLabels.size() / groupCount,
			groupSize0	= columnsWithLabels.size() - groupSize * (groupCount-1);
			

	std::vector<std::thread>	threads;
	
	size_t	curCol	= 0,
			nextEnd = groupSize0;
	
	for(size_t group=0; group < groupCount; group++)
	{
		Columns cols;
		for(; curCol < nextEnd; curCol++)
			cols.push_back(columnsWithLabels[curCol]);
		
		
		threads.push_back(std::thread([cols, group, &loadBatchOfColumns]()
		{
			loadBatchOfColumns(cols, group);
		}));
		
		nextEnd += groupSize;
	}
	
	for(std::thread & t : threads)
		t.join();
		
	transactionReadEnd();
}

void DatabaseInterface::columnSetValues(int columnId, const intvec &ints)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnSetValues ints);
	transactionWriteBegin();

	const int ds = columnGetDataSetId(columnId);
	const std::string vt = valuesTableName(ds);

	runStatements("DELETE FROM " + vt + " WHERE columnId=?;",
		[&](sqlite3_stmt *stmt){ sqlite3_bind_int(stmt, 1, columnId); });

	size_t nextRow		= 0;
	size_t rowOutside	= 0;
	bindParametersType bind = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt,   1, columnId);
		sqlite3_bind_int64(stmt, 2, (int64_t)rowOutside + 1);
		sqlite3_bind_int(stmt,   3, ints[rowOutside]);
	};

	_runStatementsRepeatedly(
		"INSERT INTO " + vt + " (columnId, rowNumber, value) VALUES (?, ?, ?)",
		[&](bindParametersType ** bp, size_t)
		{
			while(nextRow < ints.size() && ints[nextRow] == EmptyValues::missingValueInteger) nextRow++;
			if(nextRow >= ints.size()) return false;
			rowOutside = nextRow;
			nextRow++;
			*bp = &bind;
			return true;
		});

	transactionWriteEnd();
}

void DatabaseInterface::columnSetValues(int columnId, const doublevec &dbls, const stringvec &strs)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnSetValues dbls);
	transactionWriteBegin();

	const int ds = columnGetDataSetId(columnId);
	const std::string vt = valuesTableName(ds);

	runStatements("DELETE FROM " + vt + " WHERE columnId=?;",
		[&](sqlite3_stmt *stmt){ sqlite3_bind_int(stmt, 1, columnId); });

	size_t nextRow		= 0;
	size_t rowOutside	= 0;
	bindParametersType bind = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt,   1, columnId);
		sqlite3_bind_int64(stmt, 2, (int64_t)rowOutside + 1);
		if(!std::isnan(dbls[rowOutside]))
			_doubleTroubleBinder(stmt, 3, dbls[rowOutside]);
		else
			sqlite3_bind_text(stmt, 3, strs[rowOutside].c_str(), (int)strs[rowOutside].size(), SQLITE_TRANSIENT);
	};

	_runStatementsRepeatedly(
		"INSERT INTO " + vt + " (columnId, rowNumber, value) VALUES (?, ?, ?)",
		[&](bindParametersType ** bp, size_t)
		{
			while(nextRow < dbls.size() && std::isnan(dbls[nextRow]) && strs[nextRow].empty()) nextRow++;
			if(nextRow >= dbls.size()) return false;
			rowOutside = nextRow;
			nextRow++;
			*bp = &bind;
			return true;
		});

	transactionWriteEnd();
}

void DatabaseInterface::columnSetValue(int columnId, size_t row, int valueInt)
{
	const int ds = columnGetDataSetId(columnId);
	runStatements("INSERT OR REPLACE INTO " + valuesTableName(ds) + " (columnId, rowNumber, value) VALUES (?, ?, ?);",
		[&](sqlite3_stmt *stmt)
		{
			sqlite3_bind_int(stmt,   1, columnId);
			sqlite3_bind_int64(stmt, 2, (int64_t)row + 1);
			sqlite3_bind_int(stmt,   3, valueInt);
		});
}

void DatabaseInterface::columnSetValue(int columnId, size_t row, double valueDbl, const std::string & valueStr)
{
	const int ds = columnGetDataSetId(columnId);
	runStatements("INSERT OR REPLACE INTO " + valuesTableName(ds) + " (columnId, rowNumber, value) VALUES (?, ?, ?);",
		[&](sqlite3_stmt *stmt)
		{
			sqlite3_bind_int(stmt,   1, columnId);
			sqlite3_bind_int64(stmt, 2, (int64_t)row + 1);
			if(!std::isnan(valueDbl))
				_doubleTroubleBinder(stmt, 3, valueDbl);
			else
				sqlite3_bind_text(stmt, 3, valueStr.c_str(), (int)valueStr.size(), SQLITE_TRANSIENT);
		});
}

void DatabaseInterface::_doubleTroubleBinder(sqlite3_stmt * stmt, int param, double dbl)
{
	JASPTIMER_SCOPE(DatabaseInterface::_doubleTroubleBinder);
	
	if(std::isnan(dbl)	|| std::isinf(dbl))
	{
		const std::string storeThis = dbDblToString(std::isnan(dbl) ? dbDbl::nan : dbl < 0 ? dbDbl::neg_inf : dbDbl::inf);
		sqlite3_bind_text(stmt, param, storeThis.c_str(), storeThis.size(), SQLITE_TRANSIENT);
	}
	else
		sqlite3_bind_double(stmt, param, dbl);
}

double DatabaseInterface::_doubleTroubleReader(sqlite3_stmt * stmt, int colI, std::string * textReturn)
{	
	JASPTIMER_SCOPE(DatabaseInterface::_doubleTroubleReader);

	const std::string strVal = _wrap_sqlite3_column_text(stmt, colI);

	if(textReturn)
		*textReturn = "";
	
	if(!strVal.empty())
	{
		JASPTIMER_SCOPE(DatabaseInterface::_doubleTroubleReader-TRY);
		
		//Optimization:
		static const std::string _inf		= dbDblToString(dbDbl::inf);
		static const std::string _neg_inf	= dbDblToString(dbDbl::neg_inf);
		static const std::string _nan		= dbDblToString(dbDbl::nan);
		
		if(strVal == _inf)				return std::numeric_limits<double>::infinity();
		else if(strVal == _neg_inf)		return -1 * std::numeric_limits<double>::infinity();
		else if(strVal == _nan)			return EmptyValues::missingValueDouble;
		
	}
	
	double dbl = EmptyValues::missingValueDouble;
	
	if(ColumnUtils::getDoubleValue(strVal, dbl, false)) //Never use the locale of the user here, the database is always written in the C-locale
		return dbl;		// we can return the double because the textReturn is already set to empty
	
	if(textReturn)
		*textReturn = strVal;
	
	return EmptyValues::missingValueDouble;
}

size_t DatabaseInterface::columnGetLabelCount(int columnId)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnGetLabelCount);
	
	size_t out;

	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt, 1, columnId);
	};

	std::function<void(size_t row, sqlite3_stmt *stmt)> processRow = [&](size_t row, sqlite3_stmt *stmt)
	{
		int colCount = sqlite3_column_count(stmt);

		assert(colCount == 1);
		out = sqlite3_column_int(stmt, 0);

	};

	runStatements("SELECT COUNT(id) FROM Labels WHERE columnId = ?;", prepare, processRow);

	return out;
}


void DatabaseInterface::columnGetValues(int columnId, intvec & ints, const std::string & /*postFix*/)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnGetValues);
	transactionReadBegin();

	const int ds = columnGetDataSetId(columnId);
	const size_t rows = dataSetRowCount(ds);

	ints.assign(rows, EmptyValues::missingValueInteger);

	runStatements("SELECT rowNumber, value FROM " + valuesTableName(ds) +
				  " WHERE columnId=? AND rowNumber<=? ORDER BY rowNumber",
		[&](sqlite3_stmt *stmt) {
			sqlite3_bind_int(stmt,   1, columnId);
			sqlite3_bind_int64(stmt, 2, (int64_t)rows);
		},
		[&](size_t, sqlite3_stmt *stmt) {
			const int64_t rn = sqlite3_column_int64(stmt, 0);
			if(sqlite3_column_type(stmt, 1) != SQLITE_NULL && (size_t)(rn - 1) < ints.size())
				ints[(size_t)(rn - 1)] = sqlite3_column_int(stmt, 1);
		});

	transactionReadEnd();
}

void DatabaseInterface::columnGetValues(int columnId, doublevec &dbls, stringvec &strs, const std::string & /*postFix*/)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnGetValues dbls);
	transactionReadBegin();

	const int ds = columnGetDataSetId(columnId);
	const size_t rows = dataSetRowCount(ds);

	dbls.assign(rows, EmptyValues::missingValueDouble);
	strs.assign(rows, "");

	runStatements("SELECT rowNumber, value FROM " + valuesTableName(ds) +
				  " WHERE columnId=? AND rowNumber<=? ORDER BY rowNumber",
		[&](sqlite3_stmt *stmt) {
			sqlite3_bind_int(stmt,   1, columnId);
			sqlite3_bind_int64(stmt, 2, (int64_t)rows);
		},
		[&](size_t, sqlite3_stmt *stmt) {
			const int64_t rn = sqlite3_column_int64(stmt, 0);
			const size_t  i  = (size_t)(rn - 1);
			if(i >= dbls.size()) return;
			std::string s;
			double d = _doubleTroubleReader(stmt, 1, &s);
			dbls[i] = d;
			strs[i] = std::isnan(d) ? s : "";
		});

	transactionReadEnd();
}

std::string DatabaseInterface::columnBaseName(int columnId,	const std::string & postFix) const
{
	return "Column_"  + std::to_string(columnId) + (postFix.empty() ? "" : "_" + postFix);
}

std::string DatabaseInterface::dataSetName(int dataSetId) const
{
	assert(dataSetId > -1);
	return "DataSet_"  + std::to_string(dataSetId);
}

int DatabaseInterface::dataSetIncRevision(int dataSetId)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetIncRevision);
	transactionWriteBegin();

	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt, 1, dataSetId);
	};

				runStatements(	"UPDATE DataSets SET revision=revision+1	WHERE id=?;", prepare);
	int rev =	runStatementsId("SELECT revision FROM DataSets				WHERE id=?;", prepare);

	transactionWriteEnd();

	return rev;
}

int DatabaseInterface::dataSetGetRevision(int dataSetId)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetGetRevision);
	return runStatementsId("SELECT revision FROM DataSets WHERE id=?;", [&](sqlite3_stmt *stmt) { sqlite3_bind_int(stmt, 1, dataSetId); });
}

int DatabaseInterface::dataSetGetDefaultFilter(int dataSetId)
{
	const std::string name = DEFAULT_FILTER_NAME;

	return runStatementsId(
				"SELECT id FROM Filters WHERE dataSet=? AND name=?;", 
				[&](sqlite3_stmt *stmt) 
				{ 
					sqlite3_bind_int( stmt, 1, dataSetId); 
					sqlite3_bind_text(stmt, 2, name.c_str(), name.length(), SQLITE_TRANSIENT);
				}
			);
}

intset DatabaseInterface::dataSetGetFilters(int dataSetId)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetGetFilters);
	intset ints;
			
	runStatements(
				"SELECT id FROM Filters WHERE dataSet=?;", 
				[&](sqlite3_stmt *stmt) { sqlite3_bind_int(stmt, 1, dataSetId); },
				[&](size_t row, sqlite3_stmt * stmt){ ints.insert( sqlite3_column_int(stmt, 0)); });
	
	return ints;
}

stringset DatabaseInterface::dataSetFilterNames(int dataSetId)
{
	stringset filterNames;

	std::function<void(size_t row, sqlite3_stmt *stmt)> processRow = [&](size_t row, sqlite3_stmt *stmt)
	{
		int colCount = sqlite3_column_count(stmt);

		assert(colCount == 1);

		filterNames.insert(_wrap_sqlite3_column_text(stmt, 0));

	};
	
	runStatements("SELECT name FROM Filters WHERE dataSet=?;", [&](sqlite3_stmt *stmt) { sqlite3_bind_int(stmt, 1, dataSetId); }, processRow);
	
	return filterNames;
}

std::string DatabaseInterface::filterTableName(int filterIndex) const
{
	JASPTIMER_SCOPE(DatabaseInterface::filterName);
	return "Filter_"  + std::to_string(filterIndex);
}


void DatabaseInterface::columnDelete(int columnId, bool cleanUpRest)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnDelete);
	transactionWriteBegin();

	const int dataSetId		= columnGetDataSetId(columnId);
	const int columnIndex	= cleanUpRest ? columnIndexForId(columnId) : -1;

	runStatements("DELETE FROM " + valuesTableName(dataSetId) + " WHERE columnId=?;",
		[&](sqlite3_stmt *stmt){ sqlite3_bind_int(stmt, 1, columnId); });
	runStatements("DELETE FROM Labels WHERE columnId=?;",
		[&](sqlite3_stmt *stmt){ sqlite3_bind_int(stmt, 1, columnId); });
	runStatements("DELETE FROM Columns WHERE dataSet=? AND id=?;", [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt, 1, dataSetId);
		sqlite3_bind_int(stmt, 2, columnId);
	});

	if(cleanUpRest)
		columnIndexDecrements(dataSetId, columnIndex);

	transactionWriteEnd();
}

void DatabaseInterface::columnSetType(int columnId, columnType colType)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnSetType);
	std::string colT = columnTypeToString(colType);
	runStatements("UPDATE Columns SET columnType=? WHERE id=?;", [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_text(stmt, 1, colT.c_str(), colT.length(), SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt,	2, columnId);
	});
}

void DatabaseInterface::columnSetAutoSort(int columnId, bool sort)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnSetAutoSort);
	runStatements("UPDATE Columns SET autoSortByValue=? WHERE id=?;", [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_int(stmt,	1,	sort);
		sqlite3_bind_int(stmt,	2,	columnId);
	});
}

void DatabaseInterface::columnSetHasLabels(int columnId, bool hasLabels)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnSetHasLabels);
	runStatements("UPDATE Columns SET hasLabels=? WHERE id=?;", [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_int(stmt,	1,	hasLabels);
		sqlite3_bind_int(stmt,	2,	columnId);
	});
}

void DatabaseInterface::columnSetInvalidated(int columnId, bool invalidated)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnSetInvalidated);
	runStatements("UPDATE Columns SET invalidated=? WHERE id=?;", [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_int(stmt,	1,	invalidated);
		sqlite3_bind_int(stmt,	2,	columnId);
	});
}

void DatabaseInterface::columnSetDropLevels(int columnId, int dropLevels)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnSetDropLevels);
	runStatements("UPDATE Columns SET dropLevels=? WHERE id=?;", [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_int(stmt,	1,	dropLevels);
		sqlite3_bind_int(stmt,	2,	columnId);
	});
}

void DatabaseInterface::columnSetIndex(int columnId, int index)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnSetIndex);

	runStatements("UPDATE Columns SET colIdx=? WHERE id=?;", [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_int(stmt,	1, index);
		sqlite3_bind_int(stmt,	2, columnId);
	});
}

int DatabaseInterface::columnIncRevision(int columnId)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnIncRevision);
	transactionWriteBegin();

	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt, 1, columnId);
	};

				runStatements(	"UPDATE Columns SET revision=revision+1	WHERE id=?;", prepare);
	int rev =	runStatementsId("SELECT revision FROM Columns			WHERE id=?;", prepare);

	transactionWriteEnd();

	return rev;
}

int DatabaseInterface::columnGetRevision(int columnId)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnGetRevision);
	return runStatementsId("SELECT revision FROM Columns WHERE id=?;", [&](sqlite3_stmt *stmt) { sqlite3_bind_int(stmt, 1, columnId); });
}


void DatabaseInterface::columnSetName(int columnId, const std::string &name)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnSetName);
	runStatements("UPDATE Columns SET name=? WHERE id=?;", [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_text(stmt, 1, name.c_str(), name.length(), SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt,	2, columnId);
	});
}

void DatabaseInterface::columnSetTitle(int columnId, const std::string & title)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnSetTitle);
	runStatements("UPDATE Columns SET title=? WHERE id=?;", [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_text(stmt, 1, title.c_str(), title.length(), SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt,	2, columnId);
	});
}

void DatabaseInterface::columnSetEmptyVals(int columnId, const std::string & emptyValsJson)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnSetTitle);
	runStatements("UPDATE Columns SET emptyValuesJson=? WHERE id=?;", [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_text(stmt, 1, emptyValsJson.c_str(), emptyValsJson.length(), SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt,	2, columnId);
	});
}

void DatabaseInterface::columnSetDescription(int columnId, const std::string & description)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnSetDescription);
	runStatements("UPDATE Columns SET description=? WHERE id=?;", [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_text(stmt, 1, description.c_str(), description.length(), SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt,	2, columnId);
	});
}

void DatabaseInterface::columnSetComputeFilter(int columnId, const std::string &filter)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnSetComputeFilter);
	runStatements("UPDATE Columns SET computeFilter=? WHERE id=?;", [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_text(stmt, 1, filter.c_str(), filter.length(), SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt,	2, columnId);
	});
}

void DatabaseInterface::columnSetComputedInfo(int columnId, int analysisId, bool invalidated, computedColumnType codeType, const std::string & rCode, const std::string & error, const std::string & constructorJsonStr, const std::string & computeFilter)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnSetComputedInfo);

	runStatements("UPDATE Columns SET invalidated=?, codeType=?, rCode=?, error=?, constructorJson=?, analysisId=?, computeFilter=? WHERE id=?;", [&](sqlite3_stmt * stmt)
	{
		std::string codeT = computedColumnTypeToString(codeType);

		sqlite3_bind_int(stmt,  1, int(invalidated));
		sqlite3_bind_text(stmt, 2, codeT.c_str(),				codeT.length(),					SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, rCode.c_str(),				rCode.length(),					SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, error.c_str(),				error.length(),					SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, constructorJsonStr.c_str(),	constructorJsonStr.length(),	SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt,  6, analysisId);
		sqlite3_bind_text(stmt, 7, computeFilter.c_str(),		computeFilter.length(),			SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt,  8, columnId);
	});
}

void DatabaseInterface::columnGetBasicInfo(int columnId, std::string &name, std::string &title, std::string &description, columnType &colType, int & revision, Json::Value & emptyValuesJson, bool & autoSort, int & dropLevels, bool & hasLabels)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnGetBasicInfo);
	
	emptyValuesJson = Json::nullValue;
	
	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt, 1, columnId);
	};
	
	std::function<void(size_t row, sqlite3_stmt *stmt)> processRow = [&](size_t row, sqlite3_stmt *stmt)
	{
		int colCount = sqlite3_column_count(stmt);

		assert(colCount == 9);
					name			= _wrap_sqlite3_column_text(stmt, 0);
					title			= _wrap_sqlite3_column_text(stmt, 1);
					description		= _wrap_sqlite3_column_text(stmt, 2);
		std::string colTypeStr		= _wrap_sqlite3_column_text(stmt, 3);
					revision		= sqlite3_column_int(		stmt, 4);
		std::string	emptyValuesStr	= _wrap_sqlite3_column_text(stmt, 5);
					autoSort		= sqlite3_column_int(		stmt, 6);
					dropLevels		= sqlite3_column_int(		stmt, 7);
					hasLabels		= sqlite3_column_int(		stmt, 8);
					

		colType = colTypeStr.empty() ? columnType::unknown : columnTypeFromString(colTypeStr);
		
		if(colType == columnType::nominalText)
			colType = columnType::nominal;
		
		Json::Reader().parse(emptyValuesStr, emptyValuesJson);
	};

	runStatements("SELECT name, title, description, columnType, revision, emptyValuesJson, autoSortByValue, dropLevels, hasLabels FROM Columns WHERE id = ?;", prepare, processRow);
}


std::string DatabaseInterface::_wrap_sqlite3_column_text(sqlite3_stmt * stmt, int iCol)
{
	JASPTIMER_SCOPE(DatabaseInterface::_wrap_sqlite3_column_text);
	const unsigned char * col = sqlite3_column_text(stmt,	iCol);
	
	return !col ? "" : std::string(reinterpret_cast<const char*>(col));	
}

void DatabaseInterface::columnGetComputedInfo(int columnId, int &analysisId, bool &invalidated, computedColumnType &codeType, std::string &rCode, std::string &error, Json::Value &constructorJson, std::string & computeFilter)
{
	JASPTIMER_SCOPE(DatabaseInterface::columnGetComputedInfo);

	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt, 1, columnId);
	};

	std::function<void(size_t row, sqlite3_stmt *stmt)> processRow = [&](size_t row, sqlite3_stmt *stmt)
	{
		int colCount = sqlite3_column_count(stmt);

		assert(colCount == 7);

					invalidated			= sqlite3_column_int(		stmt,	0);
		std::string codeTypeStr			= _wrap_sqlite3_column_text(stmt,	1);
					rCode				= _wrap_sqlite3_column_text(stmt,	2);
					error				= _wrap_sqlite3_column_text(stmt,	3);
		std::string constructorJsonStr	= _wrap_sqlite3_column_text(stmt,	4);
					analysisId			= sqlite3_column_int(		stmt,	5);
					computeFilter		= _wrap_sqlite3_column_text(stmt,	6);

		codeType = computedColumnType::notComputed;
		if (!codeTypeStr.empty())
		{
			try { codeType = computedColumnTypeFromString(codeTypeStr); }
			catch(...) {}
		}

		constructorJson = Json::objectValue;
		Json::Reader().parse(constructorJsonStr, constructorJson);
	};

	runStatements("SELECT invalidated, codeType, rCode, error, constructorJson, analysisId, computeFilter FROM Columns WHERE id = ?;", prepare, processRow);
}

void DatabaseInterface::labelsClear(int columnId)
{
	JASPTIMER_SCOPE(DatabaseInterface::labelsClear);
	runStatements("DELETE FROM Labels WHERE columnId = ?;", [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt,	1, columnId);
	});
}

bool DatabaseInterface::labelExists(int	columnId, int intsId)
{
	JASPTIMER_SCOPE(DatabaseInterface::labelExists);
	return -1 != runStatementsId("SELECT id FROM Labels WHERE columnId = ? and value = ?;", [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt,	1, columnId);
		sqlite3_bind_int(stmt,  2, intsId);
	});
}

intset DatabaseInterface::labelsExisting(int columnId)
{
	JASPTIMER_SCOPE(DatabaseInterface::labelsExisting);
	intset ints;
			
	runStatements(
				"SELECT value FROM Labels WHERE columnId = ?;", 
				[&](sqlite3_stmt *stmt) { sqlite3_bind_int(stmt, 1, columnId); },
				[&](size_t row, sqlite3_stmt * stmt){ ints.insert( sqlite3_column_int(stmt, 0)); });
	
	return ints;
}

int DatabaseInterface::labelAdd(int columnId, int value, const std::string & label, bool filterAllows, const	std::string & description, const std::string & originalValueJson)
{
	JASPTIMER_SCOPE(DatabaseInterface::labelAdd);
	return runStatementsId("INSERT INTO Labels (columnId, value, label, filterAllows, description, originalValueJson) "
						   "VALUES (?, ?, ?, ?, ?, ?) RETURNING rowid;", [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int( stmt,	1, columnId);
		sqlite3_bind_int( stmt,	2, value);
		sqlite3_bind_text(stmt, 3, label.c_str(),				label.length(),				SQLITE_TRANSIENT);
		sqlite3_bind_int( stmt,	4, filterAllows);
		sqlite3_bind_text(stmt, 5, description.c_str(),			description.length(),		SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 6, originalValueJson.c_str(),	originalValueJson.length(),	SQLITE_TRANSIENT);
	});
}

void DatabaseInterface::labelSet(int id, int columnId, int value, const std::string & label, bool filterAllows, const	std::string & description, const std::string & originalValueJson, bool userAdded)
{
	JASPTIMER_SCOPE(DatabaseInterface::labelSet);
	runStatements("UPDATE Labels SET columnId=?, value=?, label=?, filterAllows=?, description=?, originalValueJson=?, userAdded=? "
						   "WHERE id = ?;", [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int( stmt,	1, columnId);
		sqlite3_bind_int( stmt,	2, value);
		sqlite3_bind_text(stmt, 3, label.c_str(),				label.length(),					SQLITE_TRANSIENT);
		sqlite3_bind_int( stmt,	4, filterAllows);
		sqlite3_bind_text(stmt, 5, description.c_str(),			description.length(),			SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 6, originalValueJson.c_str(),	originalValueJson.length(),		SQLITE_TRANSIENT);
		sqlite3_bind_int( stmt,	7, userAdded);
		sqlite3_bind_int( stmt,	8, id);
	});
}

void DatabaseInterface::labelDelete(int id)
{
	JASPTIMER_SCOPE(DatabaseInterface::labelDelete);
	runStatements("DELETE FROM Labels WHERE id = ?;", [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int( stmt,	1, id);
	});
}

void DatabaseInterface::labelLoad(int id, int & columnId, int & value, std::string & label, bool & filterAllows, std::string & description, std::string & originalValueJson, int & order, bool & userAdded)
{
	JASPTIMER_SCOPE(DatabaseInterface::labelLoad);
	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt, 1, id);
	};

	std::function<void(size_t row, sqlite3_stmt *stmt)> processRow = [&](size_t row, sqlite3_stmt *stmt)
	{
		int colCount = sqlite3_column_count(stmt);

		assert(colCount == 8);

					columnId			= sqlite3_column_int(stmt,			0);
					value				= sqlite3_column_int(stmt,			1);
					label				= _wrap_sqlite3_column_text(stmt,	2);
					order				= sqlite3_column_int(stmt,			3);
					filterAllows		= sqlite3_column_int(stmt,			4);
					description			= _wrap_sqlite3_column_text(stmt,	5);
					originalValueJson	= _wrap_sqlite3_column_text(stmt,	6);
					userAdded			= sqlite3_column_int(stmt,			7);

	};

	runStatements("SELECT columnId, value, label, ordering, filterAllows, description, originalValueJson, userAdded FROM Labels WHERE id = ?;", prepare, processRow);
}

void DatabaseInterface::labelsSetOrder(const intintmap & orderPerDbId)
{
	JASPTIMER_SCOPE(DatabaseInterface::labelsSetOrder);
	
	auto iter = orderPerDbId.begin();
	if(iter == orderPerDbId.end())
		return;
	
	transactionWriteBegin();
	
	bindParametersType _bindParams = [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_int(stmt, 1, iter->second);
		sqlite3_bind_int(stmt, 2, iter->first);
		iter++;
	};

	_runStatementsRepeatedly("UPDATE Labels SET ordering=? WHERE id=?", [&](bindParametersType ** bindParams, size_t)
	{
		(*bindParams) = &_bindParams;
				
		return iter != orderPerDbId.end();
	});


	transactionWriteEnd();
}

void DatabaseInterface::labelSetOrder(int id, int order)
{
	JASPTIMER_SCOPE(DatabaseInterface::labelSetOrder);
	transactionWriteBegin();

	runStatements("UPDATE Labels SET ordering=? WHERE id=?", [&](sqlite3_stmt * stmt)
	{
		sqlite3_bind_int(stmt, 1, order);
		sqlite3_bind_int(stmt, 2, id);
	});

	transactionWriteEnd();
}


void DatabaseInterface::labelsLoad(Column * column)
{
	JASPTIMER_SCOPE(DatabaseInterface::labelsLoad);
	
	transactionReadBegin();
	
	column->beginBatchedLabelsDB();
	
	int labelsSize = 0;

	column->_resetLabelValueMap();
	
	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt, 1, column->id());
	};
	
	Json::Reader reader;

	std::function<void(size_t row, sqlite3_stmt *stmt)> processRow = [&](size_t, sqlite3_stmt *stmt)
	{
		int colCount = sqlite3_column_count(stmt);

		assert(colCount == 7);

		int			id						= sqlite3_column_int(stmt,			0);
		int			value					= sqlite3_column_int(stmt,			1);
		std::string	label					= _wrap_sqlite3_column_text(stmt,	2);
		int			order					= sqlite3_column_int(stmt,			3);
		bool		filterAllows			= sqlite3_column_int(stmt,			4);
		std::string	description				= _wrap_sqlite3_column_text(stmt,	5);
		std::string	originalValueJsonStr	= _wrap_sqlite3_column_text(stmt,	6);
		
		Json::Value originalValueJson;
		
		reader.parse(originalValueJsonStr, originalValueJson);

		if (originalValueJson.isNull() && !originalValueJsonStr.empty())
			originalValueJson = originalValueJsonStr; // For backward compatibility: in some JASP files the originalValueJson is not a json string but just the original string.

		column->labelsSet(labelsSize++,	value, label, filterAllows, description, originalValueJson, order, id);
	};

	runStatements("SELECT id, value, label, ordering, filterAllows, description, originalValueJson FROM Labels WHERE columnId = ? ORDER BY ordering;", prepare, processRow);

	column->labelsShrinkOnlyToSize(labelsSize);
	 
	column->endBatchedLabelsDB(false);
	
	transactionReadEnd();
}

void DatabaseInterface::labelsLoad(const Columns &columns)//, std::function<void (float)> progressCallback)
{
	JASPTIMER_SCOPE(DatabaseInterface::labelsLoad);
	
	transactionReadBegin();
	
	std::map<int,Column*>	localColMap;
	intintmap				labelsPerCol;
	std::stringstream		statement;
	
	statement << "SELECT id, value, label, ordering, filterAllows, description, originalValueJson, columnId FROM Labels WHERE columnId IN (";
	
	for(Column * column : columns)
	{
		column->beginBatchedLabelsDB();
		
		if(localColMap.size() > 0)
			statement << ", ";
		statement << column->id();
		
		localColMap [column->id()] = column;
		labelsPerCol[column->id()] = 0;

		column->_resetLabelValueMap();
	}
	
	statement << ")  ORDER BY columnId, ordering;";

	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
	};
	
	Json::Reader reader;

	std::function<void(size_t row, sqlite3_stmt *stmt)> processRow = [&](size_t row, sqlite3_stmt *stmt)
	{
		int colCount = sqlite3_column_count(stmt);

		assert(colCount == 8);

		int			id						= sqlite3_column_int(stmt,			0);
		int			value					= sqlite3_column_int(stmt,			1);
		std::string	label					= _wrap_sqlite3_column_text(stmt,	2);
		int			order					= sqlite3_column_int(stmt,			3);
		bool		filterAllows			= sqlite3_column_int(stmt,			4);
		std::string	description				= _wrap_sqlite3_column_text(stmt,	5);
		std::string	originalValueJsonStr	= _wrap_sqlite3_column_text(stmt,	6);
		int			columnId				= sqlite3_column_int(stmt,			7);
		
		Json::Value originalValueJson;
		
		assert(id != -1);
		
		reader.parse(originalValueJsonStr, originalValueJson);

		if (originalValueJson.isNull() && !originalValueJsonStr.empty())
			originalValueJson = originalValueJsonStr; // For backward compatibility: in some JASP files the originalValueJson is not a json string but just the original string.

		if(localColMap.count(columnId))
		{
			//std::stringstream str;
			//str << "labelsLoad for column " << localColMap.at(columnId)->name() << " in thread " << std::this_thread::get_id() << " got "
			//	<< "labelsSet(row='"		<< order
			//	<< "', value='"				<< value
			//	<< "', label='"				<< label
			//	<< "', filterAllows='"		<< filterAllows
			//	<< "', description='"		<< description
			//	<< "', originalValueJson='"	<< originalValueJson
			//	<< "', order='"				<< order
			//	<< "', id='"				<< id
			//	<< "')\n";
			//Log::log() << str.str() << std::flush;

			localColMap.at(columnId)->labelsSet(labelsPerCol[columnId]++,	value, label, filterAllows, description, originalValueJson, order, id);
		}
	};

	runStatements(statement.str(), prepare, processRow);

	for(Column * column : columns)
	{
		column->labelsShrinkOnlyToSize(labelsPerCol[column->id()]);
		column->endBatchedLabelsDB(false);
	}
	
	transactionReadEnd();
}

void DatabaseInterface::labelsWrite(Column *column)
{
	JASPTIMER_SCOPE(DatabaseInterface::labelsWrite Column);
	transactionWriteBegin();

	runStatements("DELETE From Labels WHERE columnId=?", [&](sqlite3_stmt *stmt) { sqlite3_bind_int( stmt,	1, column->id()); });
	
	auto labelIter = column->labels().begin();
	
	if(labelIter != column->labels().end())
	{

		bindParametersType _bindParams =  [&](sqlite3_stmt *stmt)
		{
			const Label			*	label			= *labelIter;
			const std::string		labelDisplay	= label->label(),
									origValJson		= label->originalValue().toStyledString();
					
			sqlite3_bind_int( stmt,	1, column->id());
			sqlite3_bind_int( stmt,	2, label->intsId());
			sqlite3_bind_text(stmt, 3, labelDisplay.c_str(),			labelDisplay.length(),				SQLITE_TRANSIENT);
			sqlite3_bind_int( stmt,	4, label->filterAllows());
			sqlite3_bind_text(stmt, 5, label->description().c_str(),	label->description().length(),		SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 6, origValJson.c_str(),				origValJson.length(),				SQLITE_TRANSIENT);
			sqlite3_bind_int( stmt,	7, label->order());
		};
	
		std::function<void(size_t,size_t, sqlite3_stmt*)> processRow = [&](size_t row, size_t rep, sqlite3_stmt * stmt)
		{
			assert(sqlite3_column_count(stmt) == 1);
	
			Label * label = *labelIter;

			label->setDbId(sqlite3_column_int(stmt, 0));
	
			labelIter++;
		};
		
		_runStatementsRepeatedly("INSERT INTO Labels (columnId, value, label, filterAllows, description, originalValueJson, ordering) VALUES (?, ?, ?, ?, ?, ?, ?) RETURNING id;", [&](bindParametersType ** bindParams, size_t)
			{
				(*bindParams) = &_bindParams;
				
				return labelIter != column->labels().end();
			}, & processRow);
	}
	
	transactionWriteEnd();
}

void DatabaseInterface::labelsWrite(const Columns & columns, std::function<void(float)> progressCallback)
{
	 JASPTIMER_SCOPE(DatabaseInterface::labelsWrite Columns);
	 transactionWriteBegin();
	 
	 Labels allLabels;
	 
	 if(columns.size() > 0)
	 {
		 std::stringstream statement;
		 int count=0;
		 
		 statement << "(";
		 for(Column * column : columns)
		 {
			 if(count > 0)
				 statement << ", ";
			 statement << column->id();
			 
			 for(Label * label : column->labels())
				 allLabels.push_back(label);
			 
			 count++;
		 }
		 statement << ")";
		 
		 runStatements("DELETE From Labels WHERE columnId in " + statement.str());
	}
	 
	auto labelIter = allLabels.begin();
	
	if(labelIter != allLabels.end())
	{
		const size_t	totalRows	= allLabels.size();
		size_t			curRow		= 0,
						lastRow		= 0;
 
		 bindParametersType _bindParams =  [&](sqlite3_stmt *stmt)
		 {
			 const Label			*	label			= *labelIter;
			 const std::string			labelDisplay	= label->label(false),
										origValJson		= label->originalValue().toStyledString();
			 
			 Column					*	column			= static_cast<Column*>(label->parent());

			 sqlite3_bind_int( stmt,	1, column->id());
			 sqlite3_bind_int( stmt,	2, label->intsId());
			 sqlite3_bind_text(stmt,	3, labelDisplay.c_str(),			labelDisplay.length(),				SQLITE_TRANSIENT);
			 sqlite3_bind_int( stmt,	4, label->filterAllows());
			 sqlite3_bind_text(stmt,	5, label->description().c_str(),	label->description().length(),		SQLITE_TRANSIENT);
			 sqlite3_bind_text(stmt,	6, origValJson.c_str(),				origValJson.length(),				SQLITE_TRANSIENT);
			 sqlite3_bind_int( stmt,	7, label->order());
		 };
	 
		 std::function<void(size_t,size_t, sqlite3_stmt*)> processRow = [&](size_t row, size_t rep, sqlite3_stmt * stmt)
		 {
			 assert(sqlite3_column_count(stmt) == 1);
	 
			 Label * label = *labelIter;

			 label->setDbId(sqlite3_column_int(stmt, 0));
	 
			 labelIter++;
			 curRow++;
			 
			 if(curRow-lastRow > 100 || curRow-lastRow > totalRows / 100 || curRow >= totalRows-1)
			 {
				 progressCallback(float(curRow) / float(totalRows));
				 lastRow = curRow;
			}
		 };
		 
		 _runStatementsRepeatedly("INSERT INTO Labels (columnId, value, label, filterAllows, description, originalValueJson, ordering) VALUES (?, ?, ?, ?, ?, ?, ?) RETURNING id;", [&](bindParametersType ** bindParams, size_t)
			 {
				 (*bindParams) = &_bindParams;
				 
				 return labelIter != allLabels.end();
			 }, & processRow);
	 }
	 
	 transactionWriteEnd();
}



std::string DatabaseInterface::dbFile(bool onlyName) const
{
	JASPTIMER_SCOPE(DatabaseInterface::dbFile);

	static std::string fileName = "internal.sqlite";
	static std::string memoryName = ":memory:";

	if (_inMemory)
		return memoryName;

	return onlyName ? fileName : std::filesystem::path(TempFiles::sessionDirName() + "/" + fileName).generic_string();
}

DatabaseInterface *DatabaseInterface::singleton() 
{ 
	if(!_singleton)
	{
		Log::log() << "No DatabaseInterface::singleton available here yet, creating an interface for internal.sqlite without recreating the dbstructure" << std::endl;
		_singleton = new DatabaseInterface(false);
	}
	
	return _singleton; 
}

void DatabaseInterface::closeInterfaces()
{
	delete _singleton;
}

void DatabaseInterface::runQuery(const std::string & query, std::function<void(sqlite3_stmt *stmt)> bindParameters, std::function<void(size_t row, sqlite3_stmt *stmt)> processRow)
{
	JASPTIMER_SCOPE(DatabaseInterface::runQuery);
	runStatements(query, bindParameters, processRow);
}

void DatabaseInterface::runStatements(	const std::string & statements, bool ignoreFails)
{
	JASPTIMER_SCOPE(DatabaseInterface::runStatements);
	_runStatements(statements, nullptr, nullptr, ignoreFails);
}

void DatabaseInterface::runStatements(	const std::string & statements, std::function<void(sqlite3_stmt *stmt)>	bindParameters, bool ignoreFails)
{
	JASPTIMER_SCOPE(DatabaseInterface::runStatements);
	_runStatements(statements, &bindParameters, nullptr, ignoreFails);
}

int DatabaseInterface::runStatementsId(	const std::string & statements, std::function<void(sqlite3_stmt *stmt)>	bindParameters, bool ignoreFails)
{
	JASPTIMER_SCOPE(DatabaseInterface::runStatementsId);
	int id = -1;
	std::function<void(size_t row, sqlite3_stmt *stmt)> processId = [&](size_t row, sqlite3_stmt *stmt)
	{
		int colCount = sqlite3_column_count(stmt);

		if(colCount)
			id = sqlite3_column_int(stmt, 0);

	};

	_runStatements(statements, &bindParameters, &processId, ignoreFails);

#ifdef SIR_LOG_A_LOT
	Log::log() << "Output for '" << statements << "' returns id:" << id << std::endl;
#endif
	return id;
}

intset DatabaseInterface::runStatementsIds(	const std::string & statements, std::function<void(sqlite3_stmt *stmt)>	bindParameters, bool ignoreFails)
{
	JASPTIMER_SCOPE(DatabaseInterface::runStatementsId);
	intset ids;
	std::function<void(size_t row, sqlite3_stmt *stmt)> processId = [&](size_t row, sqlite3_stmt *stmt)
	{
		int colCount = sqlite3_column_count(stmt);

		if(colCount)
			ids.insert(sqlite3_column_int(stmt, 0));

	};

	_runStatements(statements, &bindParameters, &processId, ignoreFails);

	return ids;
}

int DatabaseInterface::runStatementsId(const std::string & statements, bool ignoreFails) 
{
	return runStatementsId(statements, [](sqlite3_stmt *stmt){}, ignoreFails);
}

intset DatabaseInterface::runStatementsIds(const std::string &statements, bool ignoreFails)
{
	return runStatementsIds(statements, [](sqlite3_stmt *stmt){}, ignoreFails);
}

void DatabaseInterface::runStatements(	const std::string & statements, std::function<void(sqlite3_stmt *stmt)>	bindParameters,	std::function<void(size_t row, sqlite3_stmt *stmt)>	processRow, bool ignoreFails)
{
	JASPTIMER_SCOPE(DatabaseInterface::runStatements);
	_runStatements(statements, &bindParameters, &processRow, ignoreFails);
}

void DatabaseInterface::workspaceUpdate(bool showRSyntax)
{
	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{
		sqlite3_bind_int(stmt,	1, showRSyntax);
		
	};

	//Log::log() << "UPDATE DataSet " << dataSetId << " with Empty Values: " << emptyValuesJson << std::endl;

	runStatements("UPDATE Workspace SET showRSyntax=?;", prepare);
}

void DatabaseInterface::workspaceLoad(bool &showRSyntax)
{
	std::function<void(sqlite3_stmt *stmt)>  prepare = [&](sqlite3_stmt *stmt)
	{};

	std::function<void(size_t row, sqlite3_stmt *stmt)> processRow = [&](size_t row, sqlite3_stmt *stmt)
	{
		int colCount = sqlite3_column_count(stmt);

		assert(colCount == 1);

		showRSyntax		= sqlite3_column_int(		stmt, 0);
	};

	runStatements("SELECT showRSyntax FROM Workspace;", prepare, processRow);
}

int DatabaseInterface::dataSetCount()
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetCount);
	return runStatementsId("SELECT COUNT(id) FROM DataSets");
}

intset DatabaseInterface::dataSetIds()
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetIds);
	return runStatementsIds("SELECT id FROM DataSets");
}

bool DatabaseInterface::dataSetExists(int dataSetId)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetExists);
	return -1 != runStatementsId("SELECT id FROM DataSets WHERE id = " + std::to_string(dataSetId) + ";");
}

void DatabaseInterface::dataSetDelete(int dataSetId)
{
	JASPTIMER_SCOPE(DatabaseInterface::dataSetDelete);
	transactionWriteBegin();

	runStatements("DELETE FROM DataSets WHERE id = " + std::to_string(dataSetId) + ";");
	runStatements("DROP TABLE IF EXISTS " + valuesTableName(dataSetId) + ";");
	runStatements("DROP TABLE IF EXISTS " + filterValuesTableName(dataSetId) + ";");
	runStatements("DELETE FROM Labels  WHERE columnId IN (SELECT id FROM Columns WHERE dataSet=" + std::to_string(dataSetId) + ");");
	runStatements("DELETE FROM Columns WHERE dataSet=" + std::to_string(dataSetId) + ";");
	runStatements("DELETE FROM Filters WHERE dataSet=" + std::to_string(dataSetId) + ";");

	transactionWriteEnd();
}

#define BUSY_MAX_SECS 90
#define BUSY_INTERVAL std::chrono::nanoseconds(1000000)

void DatabaseInterface::_runStatements(const std::string & statements, bindParametersType * bindParameters, std::function<void(size_t row, sqlite3_stmt *stmt)> * processRow, bool ignoreFails)
{
	JASPTIMER_SCOPE(DatabaseInterface::_runStatements);
#ifdef SIR_LOG_A_LOT
	Log::log() << "Running statements: '" << statements << "'" << std::endl;
#endif
	
	int64_t epochIdle = Utils::currentSeconds();

	sqlite3_stmt * dbStmt = nullptr;

	const char	*	start	= statements.c_str(),
				*	current	= start,
				*	tail;
	size_t			total	= statements.size(),
					remain,
					row;
	int				ret		= SQLITE_OK;

	do
	{
		JASPTIMER_RESUME(DatabaseInterface::_runStatements prepare);
		ret	= sqlite3_prepare_v2(_db(), current, total - (current - start), &dbStmt, &tail);
		JASPTIMER_STOP(DatabaseInterface::_runStatements prepare);
		row = 0;

		if(bindParameters)
			(*bindParameters)(dbStmt);

		JASPTIMER_RESUME(DatabaseInterface::_runStatements run statements);
		if(ret == SQLITE_OK && dbStmt)
		{
			do
			{
				ret = sqlite3_step(dbStmt);
				
				switch(ret)
				{
				case SQLITE_ERROR:
				{
					std::string errorMsg = "Running ```\n"+statements.substr(current - start)+"\n``` failed because of: `" + sqlite3_errmsg(_db());
					Log::log() << errorMsg << std::endl;
					if(!ignoreFails)
						throw std::runtime_error(errorMsg);
				}
				 
			   case SQLITE_ROW:
					epochIdle = Utils::currentSeconds();
					if(processRow)
						(*processRow)(row, dbStmt);
					row++;
					break;
					
				case SQLITE_BUSY:
					if(Utils::currentSeconds() - epochIdle > BUSY_MAX_SECS)
						throw std::runtime_error("Sqlite was busy for too long!");
					std::this_thread::sleep_for(BUSY_INTERVAL);
					break;
					
				case SQLITE_DONE:
					break;
				
				case SQLITE_NOTADB:
				case SQLITE_CORRUPT:
				{
					Log::log() << "Database is malformed..." << std::endl;
					throw dbMalformedException();
				}

					
				//case SQLITE_CORRUPT:
				//{
				//	std::string errorMsg = "Running ```\n"+statements.substr(current - start)+"\n``` with status "+std::to_string(ret)+" failed  because of: `" + sqlite3_errmsg(_db());
				//	Log::log() << errorMsg << std::endl;
				//	throw std::runtime_error(errorMsg);
				//}
				//	break;
					
				default:
				{
					std::string errorMsg = "Running ```\n"+statements.substr(current - start)+"\n``` had unchecked status "+std::to_string(ret)+" because of: `" + sqlite3_errmsg(_db());
					Log::log() << errorMsg << std::endl;
					break;
				}
			   }
				
				

			}
			while((ret == SQLITE_BUSY || ret == SQLITE_ROW) && ret != SQLITE_DONE);

			JASPTIMER_STOP(DatabaseInterface::_runStatements run statements);

			ret = sqlite3_finalize(dbStmt);
			dbStmt = nullptr;
		}

		remain	= total - (tail - start);
		//Log::log() << "Just ran `" + std::string(current, tail) + "` which returned " << ret << " and " << remain << " remaining." << std::endl;
		current	= tail;
	}
	while(remain > 1 && (ret == SQLITE_OK && ret != SQLITE_DONE));

	const int maxLenStatementError = 200;
	std::string shortStatements = statements.size() <= maxLenStatementError ? statements : statements.substr(0, maxLenStatementError);
	
	if(ret == SQLITE_ERROR)
	{
		Log::log() <<					"Running ```\n"+statements		+"\n``` failed because of: `" + sqlite3_errmsg(_db()) << std::endl;
		if(!ignoreFails)
			throw std::runtime_error(	"Running ```\n"+shortStatements	+"\n``` failed because of: `" + sqlite3_errmsg(_db()));
	}

	if(ret == SQLITE_READONLY)
	{
		Log::log() <<				"Running ```\n"+statements		+"\n``` failed because the database is readonly..." << std::endl;
		throw std::runtime_error(	"Running ```\n"+shortStatements	+"\n``` failed because the database is readonly...");
	}
}

void DatabaseInterface::_runStatementsRepeatedly(const std::string & statements, std::function<bool(bindParametersType ** bindParameters, size_t row)> bindParameterFactory, std::function<void(size_t row, size_t repetition, sqlite3_stmt *stmt)> * processRow, bool ignoreFails)
{
	JASPTIMER_SCOPE(DatabaseInterface::_runStatementsRepeatedly);
#ifdef SIR_LOG_A_LOT
	Log::log() << "Running statements repeatedly: '" << statements << "'" << std::endl;
#endif

	int64_t epochIdle = Utils::currentSeconds();
	
	sqlite3_stmt * dbStmt = nullptr;

	const char	*	start		= statements.c_str(),
				*	current		= start,
				*	tail;
	size_t			total		= statements.size(),
					remain,
					row			= 0,
					repetition	=  0;
	int				ret			= SQLITE_OK;

	std::function<void(sqlite3_stmt *stmt)> * bindParameters = nullptr;

	do
	{
		ret	= sqlite3_prepare_v2(_db(), current, total - (current - start), &dbStmt, &tail);

		row = 0;

		while((ret == SQLITE_OK || ret == SQLITE_DONE) && dbStmt && bindParameterFactory(&bindParameters, row))
		{
			if(bindParameters)
				(*bindParameters)(dbStmt);

			if((ret == SQLITE_OK || ret == SQLITE_DONE) && dbStmt)
			{
				do
				{
					ret = sqlite3_step(dbStmt);

					switch(ret)
					{
					case SQLITE_ERROR:
					{
						std::string errorMsg = "Running `\n"+statements.substr(current - start)+"\n` repeatedly failed because of: `" + sqlite3_errmsg(_db());
						Log::log() << errorMsg << std::endl;
						
						if(!ignoreFails)
							throw std::runtime_error(errorMsg);
					}

					case SQLITE_ROW:
						if(processRow)
							(*processRow)(row, repetition, dbStmt);
						row++;
						epochIdle = Utils::currentSeconds();
						break;
						
					case SQLITE_BUSY:
						if(Utils::currentSeconds() - epochIdle > BUSY_MAX_SECS)
							throw std::runtime_error("Sqlite was busy for too long!");
						std::this_thread::sleep_for(BUSY_INTERVAL);
						break;
						
					case SQLITE_DONE:
						row++;
						break;
					
					case SQLITE_NOTADB:
					case SQLITE_CORRUPT:
					{
						Log::log() << "Database is malformed..." << std::endl;
						throw dbMalformedException();
					}
						
					//case SQLITE_CORRUPT:
					//{
					//	std::string errorMsg = "Running ```\n"+statements.substr(current - start)+"\n``` with status "+std::to_string(ret)+" failed  because of: `" + sqlite3_errmsg(_db());
					//	Log::log() << errorMsg << std::endl;
					//	throw std::runtime_error(errorMsg);
					//}
						break;
						
					default:
					{
						std::string errorMsg = "Running ```\n"+statements.substr(current - start)+"\n``` had unchecked status "+std::to_string(ret)+" because of: `" + sqlite3_errmsg(_db());
						Log::log() << errorMsg << std::endl;
					}
					}
				
					
				}
				while((ret == SQLITE_BUSY || ret == SQLITE_ROW) && ret != SQLITE_DONE);
			}
			
			sqlite3_reset(dbStmt);
			repetition++;
		}
		
		if(ret == SQLITE_ERROR)
		{
			std::string errorMsg = "A problem occured trying to prepare statement `" + statements + "` and the error was: : `" + sqlite3_errmsg(_db());
			Log::log() << errorMsg << std::endl;
			throw std::runtime_error(errorMsg);
		}

		ret = sqlite3_finalize(dbStmt);
		dbStmt = nullptr;


		remain	= total - (tail - start);
		//Log::log() << "Just ran `" + std::string(current, tail) + "` which returned " << ret << " and " << remain << " remaining." << std::endl;
		current	= tail;
	}
	while(remain > 1 && (ret == SQLITE_OK && ret != SQLITE_DONE));

	if(ret == SQLITE_ERROR)
	{
		std::string errorMsg = "Running ```\n"+statements+"\n``` failed because of: `" + sqlite3_errmsg(_db());
		Log::log() << errorMsg << std::endl;

		if(!ignoreFails)
			throw std::runtime_error(errorMsg);
	}

	if(ret == SQLITE_READONLY)
	{
		std::string errorMsg = "Running ```\n"+statements+"\n``` failed because the database is readonly...";
		Log::log() << errorMsg << std::endl;
		throw std::runtime_error(errorMsg);
	}
}

sqlite3 * DatabaseInterface::_db()
{	
	const auto id = std::this_thread::get_id();

	if(_dbCreated && _dbCreator == id)
		return _dbCreated;

	_dbCheckMutex.lock();
	bool itsNotThereYet = !_dbs.count(id); //tip toe around the map
	_dbCheckMutex.unlock();

	if(itsNotThereYet)
		load();

	_dbCheckMutex.lock();
	sqlite3 * dbFound = _dbs.at(id);
	_dbCheckMutex.unlock();

	return dbFound;
}

void DatabaseInterface::_migrateToValueSchema(Version originalVersion)
{
	const bool keepLegacyForDo019Fix = (originalVersion != Version() && originalVersion < "0.19");
	const intset ids = dataSetIds();
	
	for(int ds : ids)
	{
		const std::string oldTable		= dataSetName(ds);
		const std::string newTable		= valuesTableName(ds);
		const std::string newFilterTbl	= filterValuesTableName(ds);
	
		if(!tableExists(newTable))
			runStatements("CREATE TABLE " + newTable +
				" (columnId INTEGER NOT NULL, rowNumber INTEGER NOT NULL, value NUM, "
				"  PRIMARY KEY (columnId, rowNumber)) WITHOUT ROWID;");
	
		if(!tableExists(newFilterTbl))
			runStatements("CREATE TABLE " + newFilterTbl +
				" (filterId INTEGER NOT NULL, rowNumber INTEGER NOT NULL, value INT NOT NULL DEFAULT 1, "
				"  PRIMARY KEY (filterId, rowNumber)) WITHOUT ROWID;");
	
		// Backfill rowCount from the wide table if we still have it.
		if(tableExists(oldTable))
		{
			const int rc = runStatementsId("SELECT COUNT(*) FROM " + oldTable + ";");
			runStatements("UPDATE DataSets SET rowCount=? WHERE id=?;",
				[&](sqlite3_stmt *s){ sqlite3_bind_int(s, 1, rc); sqlite3_bind_int(s, 2, ds); });
		}
	
		// do019Fix needs the legacy wide table; skip the data migration, keep the table.
		if(keepLegacyForDo019Fix)
			continue;
	
		if(!tableExists(oldTable))
			continue; // already migrated
	
		// Columns -> values
		intset colIds;
		runStatements(
			"SELECT id FROM Columns WHERE dataSet=? ORDER BY colIdx;",
			[&](sqlite3_stmt *s){ sqlite3_bind_int(s, 1, ds); },
			[&](size_t, sqlite3_stmt *s){ colIds.insert(sqlite3_column_int(s, 0)); });
	
		for(int colId : colIds)
		{
			const std::string cn = "Column_" + std::to_string(colId);
			if(!tableHasColumn(oldTable, cn)) continue;
			runStatements(
				"INSERT INTO " + newTable + " (columnId, rowNumber, value) "
				"SELECT " + std::to_string(colId) + ", rowNumber, " + cn +
				" FROM " + oldTable + " WHERE " + cn + " IS NOT NULL;");
		}
	
		// Filters -> filter values (defaults are 1, so only store != 1)
		intset filterIds;
		runStatements(
			"SELECT id FROM Filters WHERE dataSet=?;",
			[&](sqlite3_stmt *s){ sqlite3_bind_int(s, 1, ds); },
			[&](size_t, sqlite3_stmt *s){ filterIds.insert(sqlite3_column_int(s, 0)); });
	
		for(int fId : filterIds)
		{
			const std::string fn = "Filter_" + std::to_string(fId);
			if(!tableHasColumn(oldTable, fn)) continue;
			runStatements(
				"INSERT INTO " + newFilterTbl + " (filterId, rowNumber, value) "
				"SELECT " + std::to_string(fId) + ", rowNumber, " + fn +
				" FROM " + oldTable + " WHERE " + fn + " != 1;");
		}
	
		runStatements("DROP TABLE " + oldTable + ";");
	}
}

// Optional: drop legacy wide tables that were deliberately kept for older files.
void DatabaseInterface::_dropLegacyWideTables()
{
for(int ds : dataSetIds())
{
	const std::string oldTable = dataSetName(ds);
	if(tableExists(oldTable))
		runStatements("DROP TABLE " + oldTable + ";");
}
}

void DatabaseInterface::create()
{
	JASPTIMER_SCOPE(DatabaseInterface::create);
	assert(!_dbCreated);

	if(!_inMemory && std::filesystem::exists(dbFile()))
	{
		Log::log() << "DatabaseInterface::create: Removing existing sqlite internal db at " << dbFile() << std::endl;
		std::filesystem::remove(dbFile());
	}
	
	int ret = sqlite3_open_v2(dbFile().c_str(), &_dbCreated, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
	
	_transactionWriteDepth = 0;
	_transactionReadDepth = 0;

	if(ret != SQLITE_OK)
	{
		Log::log() << "Couldnt open sqlite internal db (" << dbFile() << "), because of: " << (_dbCreated ? sqlite3_errmsg(_dbCreated) : "not even a broken sqlite3 obj was returned..." ) << std::endl;
		throw std::runtime_error("JASP cannot run without an internal database and it cannot be created. Contact the JASP team for help.");
	}
	else
		Log::log() << "Opened internal sqlite database for creation at '" << dbFile() << "'.  This is for thread " << std::this_thread::get_id() << std::endl;
	
	//runStatements("pragma journal_mode=wal;");
	//runStatements("pragma synchronous=full;");
	
	sqlite3_busy_timeout(_dbCreated, 100);
	
	bool	constructionWorked	= false;
	size_t	constructionAttempt = 0; 
isItReallyALabel:
	
	try
	{
		transactionWriteBegin();
		runStatements(_dbConstructionSql);
		runStatements(_dbIndexesSql);
		
		// Fallback: ensure rowCount exists even if internalDbDefinition.h was not regenerated.
		if(!tableHasColumn("DataSets", "rowCount"))
			runStatements("ALTER TABLE DataSets ADD COLUMN rowCount INT DEFAULT 0;");
		
		transactionWriteEnd();
		
		constructionWorked = true;
	}
	catch(dbMalformedException & e)
	{
		//Unfortunate, but perhaps we were too quick?	
		constructionWorked = false;
		constructionAttempt++;
	}
	
	if(!constructionWorked)
	{
		if(constructionAttempt > 10)
			throw dbMalformedException();
		
		std::this_thread::sleep_for(std::chrono::nanoseconds(1000000));
		goto isItReallyALabel;
	}
	
	_dbCreator = std::this_thread::get_id();
}

void DatabaseInterface::preloadInterfaceForThread()
{
	//Load the interface by asking for it
	_db();
}

void DatabaseInterface::load()
{
	JASPTIMER_SCOPE(DatabaseInterface::load);
	assert(!_dbCreated || std::this_thread::get_id() != _dbCreator);

	_loadMutex.lock();
	if(_dbs.count(std::this_thread::get_id()))
	{
		sqlite3* connection = _dbs.at(std::this_thread::get_id());
		_loadMutex.unlock();
		return;
	}

	if(!std::filesystem::exists(dbFile()))
		throw std::runtime_error("Trying to load '" + dbFile() + "' but it doesn't exist!");

	bool        loadingWorked	= false;
	size_t      loadingAttempt	= 0;
	sqlite3 *   db              = nullptr;

	int ret = sqlite3_open_v2(dbFile().c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL);

	if(ret != SQLITE_OK)
	{
		Log::log() << "Couldnt open sqlite internal db, because of: " << (db ? sqlite3_errmsg(db) : "not even a broken sqlite3 obj was returned..." ) << std::endl;
		throw std::runtime_error("JASP cannot run without an internal database and it cannot be created. Contact the JASP team for help.");
	}
	else
		Log::log() << "Opened internal sqlite database for loading at '" << dbFile() << "'. This is for thread " << std::this_thread::get_id() << std::endl;

	_dbCheckMutex.lock();
	_dbs[std::this_thread::get_id()] = db;
	_transactionWriteDepth = 0;
	_transactionReadDepth = 0;
	_dbCheckMutex.unlock();
	_loadMutex.unlock();

	for(bool loadingWorked = false; !loadingWorked; )
	{
		sqlite3_busy_timeout(db, 100);

		try
		{
			int tableCount = runStatementsId("SELECT COUNT(*) FROM sqlite_schema WHERE type ='table' AND name NOT LIKE 'sqlite_%';");
			if(tableCount < 0)
				throw dbMalformedException();

			Log::log() << "Loaded a database with #" << tableCount << " tables." << std::endl;
			loadingWorked = true;
		}
		catch(dbMalformedException & e)
		{
			//Unfortunate, but perhaps we were too quick?
			loadingWorked = false;
			loadingAttempt++;
		}

		if(!loadingWorked)
		{
			if(loadingAttempt > 10 * 60) //Timeout is 0.1 sec, so this lets the db try for 1 minute to connect...
				throw dbMalformedException();

			Log::log() << "There was a problem loading the database, retrying for the #" << loadingAttempt << " time" << std::endl;
			std::this_thread::sleep_for(std::chrono::nanoseconds(100000000));
		}
	}

	return;
}

void DatabaseInterface::close()
{
	JASPTIMER_SCOPE(DatabaseInterface::close);
	
	std::set<sqlite3*> waitingFor;
	
	_dbCheckMutex.lock();

	for(auto & idDb : _dbs)
		waitingFor.insert(idDb.second);
					
	do
	{
		for(auto & idDb : _dbs)
			if(waitingFor.count(idDb.second))
			{
				int ret = sqlite3_close(idDb.second);
				
				if(ret == SQLITE_OK)
					waitingFor.erase(idDb.second);
			}
		
		if(waitingFor.size() > 0)
			std::this_thread::sleep_for(std::chrono::nanoseconds(10000000));
	}
	while(waitingFor.size() > 0);
		
	_dbs.clear();

	_dbCheckMutex.unlock();
	
	while(_dbCreated && sqlite3_close(_dbCreated) != SQLITE_OK)
	{
		std::this_thread::sleep_for(std::chrono::nanoseconds(10000000));
	}
	_dbCreated = nullptr;
}

bool DatabaseInterface::tableHasColumn(const std::string &tableName, const std::string &columnName)
{
	return SQLITE_OK == sqlite3_table_column_metadata(
	  _db(),
	  NULL,     
	  tableName.c_str(), 
	  columnName.c_str(),
	  NULL,
	  NULL,
	  NULL,
	  NULL,
	  NULL
				);
}

bool DatabaseInterface::tableExists(const std::string &name)
{
	return runStatementsId("SELECT COUNT(name) FROM sqlite_master WHERE type='table' AND name='" + name + "';") > 0;
}

int DatabaseInterface::transactionWriteDepth()
{
	return _transactionWriteDepth;
}

int DatabaseInterface::transactionReadDepth()
{
	return _transactionReadDepth;
}

void DatabaseInterface::transactionWriteBegin()
{
	JASPTIMER_SCOPE(DatabaseInterface::transactionWriteBegin);
	
	if(_transactionReadDepth > 0)
	{
		Log::log() << "A read transaction seems to be still going during DatabaseInterface::transactionWriteBegin, however, instead of crashing directly we will just try to unwind it." << std::endl;
		while(_transactionReadDepth > 0) transactionReadEnd();
	}
	
	if(_transactionWriteDepth++ == 0)
		runStatements("BEGIN EXCLUSIVE", true); //runStatements already has a while loop handling SQLITE_BUSY so this should work?
}

void DatabaseInterface::transactionReadBegin()
{
	JASPTIMER_SCOPE(DatabaseInterface::transactionReadBegin);
	
	if(_transactionWriteDepth > 0)
	{
		Log::log() << "A write transaction seems to be still going during DatabaseInterface::transactionReadBegin, however, instead of crashing directly we will just try to unwind it." << std::endl;
		while(_transactionWriteDepth > 0) transactionWriteEnd();
	}
	
	if(_transactionReadDepth++ == 0)
		runStatements("BEGIN DEFERRED", true);
}

void DatabaseInterface::transactionWriteEnd(bool rollback)
{
	JASPTIMER_SCOPE(DatabaseInterface::transactionWriteEnd);
	
	if(rollback)	
	{
		runStatements("ROLLBACK");
		_transactionWriteDepth = 0;
		throw std::runtime_error("Rollback!"); //Might be better to use a subclass of std::runtime_error but for now this isnt even used anyway.
	}	
	else if(_transactionWriteDepth == 0 || --_transactionWriteDepth == 0)
		runStatements("COMMIT", true);

}

void DatabaseInterface::transactionReadEnd()
{
	JASPTIMER_SCOPE(DatabaseInterface::transactionReadEnd);
	
	if(_transactionReadDepth == 0 || --_transactionReadDepth == 0)
		runStatements("COMMIT", true); //ignore fails cause it fails sometimes, probably because no statements was done and so nothing to commit. this is ok
}

void DatabaseInterface::truncateAllTables()
{
	JASPTIMER_SCOPE(DatabaseInterface::truncateAllTables);

	for(int ds : dataSetIds())
	{
		runStatements("DROP TABLE IF EXISTS " + valuesTableName(ds) + ";");
		runStatements("DROP TABLE IF EXISTS " + filterValuesTableName(ds) + ";");
		runStatements("DROP TABLE IF EXISTS " + dataSetName(ds) + ";");
	}

	runStatements(R"MultiPower(
							DELETE FROM Labels;
							DELETE FROM Columns;
							DELETE FROM Filters;
							DELETE FROM DataSets;
							DELETE FROM Workspace;
							VACUUM;
						)MultiPower");
}


