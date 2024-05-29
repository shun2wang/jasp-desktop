//
// Copyright (C) 2013-2018 University of Amsterdam
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

#ifndef RLANGSYNTAXHIGHLIGHTER_H
#define RLANGSYNTAXHIGHLIGHTER_H

#include <QSyntaxHighlighter>
#include <QTextCursor>
#include <QRegularExpression>

class RlangSyntaxHighlighter : public QSyntaxHighlighter
{
public:
    RlangSyntaxHighlighter(QTextDocument *parent);
	virtual void highlightBlock(const QString &text) override;
private:
	struct HighlightingRule
	{
		QRegularExpression pattern;
		QTextCharFormat format;
	};
	QVector<HighlightingRule> highlightingRules;
	QTextCharFormat operatorFormat;
	QTextCharFormat variableFormat;
    QTextCharFormat commentFormat;
    QTextCharFormat keywordFormat;
    QTextCharFormat stringFormat;
    QTextCharFormat booleanFormat;
    QTextCharFormat numberFormat;
    QTextCharFormat punctuationFormat;
};

#endif // RLANGSYNTAXHIGHLIGHTER_H
