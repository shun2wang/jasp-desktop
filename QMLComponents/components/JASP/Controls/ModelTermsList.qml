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

import QtQuick
import JASP.Controls
import JASP


VariablesList
{
	id						: modelTermsList
	dropMode				: JASP.DropInsert
	name					: "modelTerms"
	title					: qsTr("Model Terms!!")
	listViewType			: JASP.Interaction
	allowTypeChange			: false

	rowComponentTitle		: qsTr("Add to null mode")
	interactionHighOrderCheckBox : "isNuisance"

	property string checkedPerDefault: "randomFactors"

	rowComponent			: CheckBox
	{
		name: "isNuisance"
		Component.onCompleted:
		{
			var varList = form.getControl(modelTermsList.checkedPerDefault)
			if ((typeof(isNew) !== 'undefined') && isNew && varList)
				checked = varList.columnsNames.includes(rowValue)
		}
	}
}
