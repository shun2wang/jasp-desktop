//
// Copyright (C) 2026 University of Amsterdam
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
#include "csvpreviewmodel.h"
#include "utilities/desktopcommunicator.h"
#include "utilities/qutils.h"

CsvPreviewModel::CsvPreviewModel(QObject *parent) : QAbstractTableModel(parent)
{
}

void CsvPreviewModel::setRawData(const QString &data)
{
	if (_rawData == data) return;
	_rawData = data;
	emit rawDataChanged();
	updateInternalStructure();
}

void CsvPreviewModel::setDelimiter(QChar delim)
{
	if (_delimiter == delim) return;
	_delimiter = delim;
	emit delimiterChanged();
	updateInternalStructure();
}

void CsvPreviewModel::setDelimiterFromChar(char delim)
{
	setDelimiter(QChar(delim));
}

void CsvPreviewModel::preparePreview(const QString &data, char delimiter)
{
	setRawData(data);
	setDelimiter(QChar(delimiter));
	setVisible(true);
}

void CsvPreviewModel::updateLocale()
{
	updateInternalStructure();
}

void CsvPreviewModel::updateInternalStructure()
{
	beginResetModel();
	_grid.clear();

	if (_rawData.isEmpty()) 
	{
			endResetModel();
			return;
	}

	parseCsvString(_rawData, _delimiter, _grid);

	endResetModel();
	emit clearTableForResize();
}

void CsvPreviewModel::parseCsvString(const QString &rawData, QChar delimiter, QList<QList<QString>> &outGrid) const
{
	enum State { Normal, Quoted, QuotedQuote };
	State state = Normal;
	QString currentField;
	QList<QString> currentRow;

	auto finishField = [&]() 
	{
		currentField.replace('\r', ' '); // No carriage returns
		currentField.replace('\n', ' '); // No newlines
		currentRow.append(currentField);
		currentField.clear();
	};

	auto finishRow = [&]() 
	{
		if (!currentField.isEmpty() || !currentRow.isEmpty() || state != Normal)
			finishField();

		if (!currentRow.isEmpty()) 
		{
			outGrid.append(currentRow);
			currentRow.clear();
		}
	};

	int i = 0;
	const int len = rawData.length();
	while (i < len) {
		QChar ch = rawData.at(i);

		switch (state) 
		{
		case Normal:
				if (ch == '"') {
					state = Quoted;
				} 
				else if (ch == delimiter) 
				{
					finishField();
				} 
				else if (ch == '\n' || ch == '\r')
				{
					finishRow();
					if (ch == '\r' && i + 1 < len && rawData.at(i + 1) == '\n')
						i++;
				}
				else 
				{
					currentField.append(ch);
				}
				break;

		case Quoted:
				if (ch == '"') 
				{
					state = QuotedQuote;
				} 
				else 
				{
					currentField.append(ch);
				}
				break;

		case QuotedQuote:
				if (ch == '"') 
				{
					currentField.append('"');   // escaped quote -> add one double quote
					state = Quoted;
				} else 
				{
					state = Normal;
					continue;
				}
				break;
		}
		i++;
	}
	finishRow();
}

int CsvPreviewModel::rowCount(const QModelIndex &) const
{
	return _grid.count();
}

int CsvPreviewModel::columnCount(const QModelIndex &) const
{
	if (_grid.isEmpty()) 
		return 0;
	
	// Find the max number of columns across all rows to ensure a rectangular grid
	int maxCols = 0;
	for (const auto &row : _grid)
		if (row.size() > maxCols) 
			maxCols = row.size();
	
	return maxCols;
}

QVariant CsvPreviewModel::data(const QModelIndex &index, int role) const
{
	if (!index.isValid() || role != Qt::DisplayRole)
		return QVariant();

	int r = index.row();
	int c = index.column();

	// Check if the row exists and if this row has a column at this index
	if (r < _grid.size() && c < _grid[r].size()) {
		QString val = _grid[r][c];

		if (val.isEmpty()) {
			if (r == 0)
				return QVariant(QString("V") + QString::number(c + 1));
			return QVariant();
		}

		if (r == 0) // Do not change the column names
			return val;

		double dblVal;
		if (QColumnUtils::getDoubleValue(val, dblVal, true))
			return QVariant(QColumnUtils::doubleToString(dblVal));

		// Add quotes to signify that this will be considered as a string
		return QVariant("\"" + val + "\"");
	}

	return QVariant();
}

QHash<int, QByteArray> CsvPreviewModel::roleNames() const
{
	QHash<int, QByteArray> roles;
	roles[Qt::DisplayRole] = "display";
	return roles;
}

bool CsvPreviewModel::visible() const
{
	return _visible;
}

void CsvPreviewModel::setVisible(bool newVisible)
{
	if (_visible == newVisible)
		return;
	
	_visible = newVisible;
	emit visibleChanged();
	
	if(!_visible)
		DesktopCommunicator::singleton()->delimiterChosen(_delimiter.toLatin1());
}
