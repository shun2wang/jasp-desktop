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

#include "rlangsyntaxhighlighter.h"

RlangSyntaxHighlighter::RlangSyntaxHighlighter(QTextDocument *parent)
	: QSyntaxHighlighter(parent)
{
	
	HighlightingRule rule;
    // all these R regExp are copied from: https://github.com/PrismJS/prism/blob/master/components/prism-r.js

	// operators
    operatorFormat.setForeground(Qt::red);
    rule.pattern = QRegularExpression(R"(->?>?|<(?:=|<?-)?|[>=!]=?|::?|&&?|\|\|?|[+*\/^$@~]|%[^%\s]*%)");
    rule.format = operatorFormat;
    highlightingRules.append(rule);
	
	// variables
	variableFormat.setToolTip("variable");
	rule.pattern = QRegularExpression("\\b\\w*\\b");
	rule.format = variableFormat;
	highlightingRules.append(rule);

    // string
    stringFormat.setForeground(Qt::darkGreen);
    rule.pattern = QRegularExpression(R"((['"])(?:\\.|(?!\1)[^\\\r\n])*\1)");
    rule.format = stringFormat;
    highlightingRules.append(rule);

    // keyword
    keywordFormat.setForeground(Qt::cyan);
    rule.pattern = QRegularExpression(R"(\b(?:NA|NA_character_|NA_complex_|NA_integer_|NA_real_|NULL|break|else|for|function|if|in|next|repeat|while)\b)");
    rule.format = keywordFormat;
    highlightingRules.append(rule);


    // boolean
    booleanFormat.setForeground(Qt::magenta);
    rule.pattern = QRegularExpression(R"(\b(?:FALSE|TRUE)\b)");
    rule.format = booleanFormat;
    highlightingRules.append(rule);

    // number
    numberFormat.setForeground(Qt::darkMagenta);
    rule.pattern = QRegularExpression(R"((?:\b0x[\dA-Fa-f]+(?:\.\d*)?|\b\d+(?:\.\d*)?|\B\.\d+)(?:[EePp][+-]?\d+)?[iL]?)");
    rule.format = numberFormat;
    highlightingRules.append(rule);

    // punctuation
    punctuationFormat.setForeground(Qt::cyan);
    rule.pattern = QRegularExpression(R"([(){}\[\],;])");
    rule.format = punctuationFormat;
    highlightingRules.append(rule);
	
	// comments
	commentFormat.setForeground(Qt::darkGray);
	commentFormat.setFontItalic(true);
    rule.pattern = QRegularExpression(R"(#.*)");
	rule.format = commentFormat;
    highlightingRules.append(rule);
}

void RlangSyntaxHighlighter::highlightBlock(const QString &text)
{
	for (const HighlightingRule &rule : highlightingRules)
	{
		QRegularExpressionMatchIterator matchIterator = rule.pattern.globalMatch(text);
		while (matchIterator.hasNext())
		{
			QRegularExpressionMatch match = matchIterator.next();
			setFormat(match.capturedStart(), match.capturedLength(), rule.format);
		}
	}
}
