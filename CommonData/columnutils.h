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
#ifndef COLUMNUTILS_H
#define COLUMNUTILS_H

#include <string>
#include <locale>
#include "utils.h"
#include <functional>

class ColumnUtils
{
public:
	typedef std::function<std::string(double, const std::string &, bool)>	currencyF;
	typedef std::function<std::string(double, int, bool)>					doubleF;
	typedef std::function<bool(std::string, double&)>						toDoubleF;
	typedef std::function<bool(std::string, int&)>							toIntF;

	friend class PreferencesModel;

	static bool					getIntValue(	const std::string	& value, int	& intValue);
	static bool					getIntValue(	const double		& value, int	& intValue);
	static bool					getDoubleValue(	const std::string	& value, double	& doubleValue,	bool useLocale = true);
	static doubleset			getDoubleValues(const stringset		& values, bool stripNAN = true);

	static bool					isIntValue(		const std::string	& value);
	static bool					isDoubleValue(	const std::string	& value);
	static std::string			doubleToLocale(	const std::string	& value);

	static void					convertEscapedUnicodeToUTF8(			std::string & inputStr);

	static std::string			doubleToString(			double dbl,		bool sepas = true, int precision = 10);
	static std::string			doubleToStringMaxPrec(	double dbl,		bool sepas = true);
	static std::string			currencyString(			double money, const std::string &symbol = std::string(),	bool sepas = true);
	
	static bool					convertVecToInt(	const stringvec & values, intvec	& intValues, intset & uniqueValues);
	static bool					convertVecToDouble(	const stringvec & values, doublevec	& doubleValues);
	
	static void					setAlternativeDoubleToString(	doubleF		newDoubleFunc, currencyF newCurrencyFunc);
	static void					setExtraStringToNumber(			toDoubleF	newDoubleFunc, toIntF newIntFunc);
	static const std::string &	decimalPoint()							{ return _decimalPoint; }
	static void					setDecimalPoint(const std::string & p)	{ _decimalPoint = p;}
	
	static const std::string &	currentQLocaleId()							{ return _currentQLocaleId; }
	static void					setCurrentQLocaleId(const std::string & p)	{ _currentQLocaleId = p;}
	
private:	
	static std::string			_convertEscapedUnicodeToUTF8(	std::string hex);
	static currencyF			_alternativeCurrencyToString;
	static doubleF				_alternativeDoubleToString;
	static toDoubleF			_extraStringToDouble;
	static toIntF				_extraStringToInt;
	static std::string			_decimalPoint,
								_currentQLocaleId;
};

#endif // COLUMNUTILS_H
