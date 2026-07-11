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
#include "csvimporter.h"
#include "csv/csvimportcolumn.h"
#include "csv/csv.h"
#include "timers.h"
#include "utilities/desktopcommunicator.h"

using namespace std;


CSVImporter::CSVImporter(bool askForDelimeter) : Importer(), _askForDelimeter{askForDelimeter}
{
}


ImportDataSet* CSVImporter::loadFile(const string &locator, std::function<void(int)> progressCallback)
{
	JASPTIMER_RESUME(CSVImporter::loadFile);

	ImportDataSet* result = new ImportDataSet(this);
	CSV csv(locator);
	csv.open();

	// Handle delimiters
	char delimiter = csv.delimiter();
	if (!_synching && _askForDelimeter)
	{
		try 
		{
			delimiter = DesktopCommunicator::singleton()->askCsvDelimiter(delimiter, QString::fromStdString(csv.firstRowsPlease()));
		} catch (...) { }

		if (!delimiter) return nullptr;
		DesktopCommunicator::singleton()->setKnownCsvDelimiter(delimiter);
	}
	else
	{
		char known = DesktopCommunicator::singleton()->knownCsvDelimiter();
		if (known != '\0') delimiter = known;
	}
	csv.setDelimiter(delimiter);

	stringvec colNames;
	csv.readLine(colNames);

	vector<CSVImportColumn *> importColumns;
	importColumns.reserve(colNames.size());

	for (size_t i = 0; i < colNames.size(); ++i)
	{
		string colName = colNames[i];

		if (colName.empty())
		{
			colName = "V" + to_string(i + 1);
		}
		else
		{
			try
			{
				if(std::to_string(std::stoi(colName)) == colName) //Check if it is a number or has more afterwards (stoi won't fail if it starts with numbers. To avoid it converting "1hahaha" into "V1hahaha")
					colName = "V" + colName;
			}
			catch (...) {}
		}

		// Check for duplicate names by searching backward in the already processed columns
		auto duplicateColName = [&colName](CSVImportColumn* col) { return col->name() == colName; };
		while (std::any_of(importColumns.begin(), importColumns.end(), duplicateColName))
		{
			colName = colNames[i] + "_" + to_string(i + 1);
		}

		importColumns.push_back(new CSVImportColumn(result, colName, csv.numRows()));
	}

	// Read data
	unsigned long long lastProgress = -1;
	size_t columnCount = importColumns.size();
	stringvec line;

	while (csv.readLine(line))
	{
		unsigned long long progress = 50 * csv.pos() / csv.size();
		if (progress != lastProgress) 
		{
			progressCallback(progress);
			lastProgress = progress;
		}

		if (line.empty()) continue; // Skip empty lines

		// Dynamically expand has more elements than columns
		if (line.size() > columnCount)
		{
			size_t currentRowCount = importColumns.empty() ? 0 : importColumns[0]->size();

			for (size_t i = columnCount; i < line.size(); ++i) {
				string newColName = "V" + to_string(i + 1);

				CSVImportColumn* newColumn = new CSVImportColumn(result, newColName, csv.numRows());

				// Pad previously read rows with empty strings to align the bounds
				for (size_t row = 0; row < currentRowCount; ++row)
					newColumn->addValue("");

				importColumns.push_back(newColumn);
			}
			columnCount = line.size();
		}

		for (size_t i = 0; i < columnCount; ++i)
			importColumns[i]->addValue(i < line.size() ? line[i] : "");

		line.clear();
	}

	for (auto* col : importColumns)
		result->addColumn(col);

	result->buildDictionary();

	JASPTIMER_STOP(CSVImporter::loadFile);
	return result;
}
