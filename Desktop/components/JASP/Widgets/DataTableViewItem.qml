import QtQuick
import QtQuick.Controls
import JASP.Controls		as JaspControls
import QtQml.Models
import QtQuick.Layouts
Item
{
					id:			itemDelegateItem
	property bool	showShadow:	itemShadowText !== undefined && itemText !== undefined && itemText !== itemShadowText
	
	TextMetrics
	{
		id:		itemDelegateTextMetrics
		font:	itemDelegateText.font
		text:	itemText === undefined ? "" : itemText
		
		
		property real specialWidth:				! showShadow
												? itemDelegateItem.width 
												: Math.max(
													Math.min(itemDelegateTextMetrics.width, itemDelegateItem.width / 2), 
													itemDelegateItem.width
												)
	}
	
	TextMetrics
	{
		id:		itemDelegateLabelMetrics
		font:	itemDelegateLabel.font
		text:	itemShadowText === undefined ? "" : itemShadowText
	}

	RowLayout
	{
		anchors.fill:				parent
		spacing:					jaspTheme.itemPadding
		
		Text
		{
			id:						itemDelegateText
			text:					itemText === undefined ? "" : itemText
			textFormat:				Text.PlainText
			color:					itemActive ? jaspTheme.textEnabled : jaspTheme.textDisabled
			font:					jaspTheme.font
			verticalAlignment:		Text.AlignVCenter
			elide:					Text.ElideRight
			horizontalAlignment:	Text.AlignLeft
			height:					parent.height
			padding:				0
			Layout.maximumWidth:	! showShadow 
									? parent.width 
									: itemDelegateLabelMetrics.width > parent.width / 2
									? parent.width / 2
									: parent.width - ( itemDelegateLabelMetrics.width + 2*jaspTheme.itemPadding)
		}
			
		Text
		{
			id:						itemDelegateLabel
			text:					itemShadowText === undefined ? "" : itemShadowText
			textFormat:				Text.PlainText
			color:					jaspTheme.textDisabled
			font:					jaspTheme.font
			verticalAlignment:		Text.AlignVCenter
			horizontalAlignment:	Text.AlignRight
			elide:					Text.ElideLeft
			visible:				showShadow
			height:					parent.height
			padding:				0
			Layout.fillWidth:		true
		}
	}

	JASPMouseAreaToolTipped
	{
		z:					1234
		hoverEnabled:		true
		anchors.fill:		itemHighlight
		acceptedButtons:	Qt.LeftButton | Qt.RightButton
		
		toolTipText:		(itemShadowText !== undefined && itemText !== undefined && itemText !== itemShadowText)
								? ((itemDelegateText.truncated || itemDelegateLabel.truncated) ? "%1 - %2".arg(itemText).arg(itemShadowText) : "")
								: (itemDelegateText.truncated ? itemText : "")

        onClicked:	(mouse) =>
		{
			if(ribbonModel.dataMode)
			{
				var shiftPressed	= Boolean(mouse.modifiers & Qt.ShiftModifier)
				var controlPressed	= Boolean(mouse.modifiers & Qt.ControlModifier)
				var rightPressed	= mouse.button === Qt.RightButton
				var isSelected		= dataTableView.view.isSelected(rowIndex, columnIndex)
					
				if(!rightPressed)
					dataTableView.view.select(rowIndex, columnIndex, shiftPressed, controlPressed);
				else
				{
					dataTableView.view.clearEdit()
					dataTableView.showPopupMenu(itemHighlight, mapToGlobal(mouse.x, mouse.y), rowIndex, columnIndex);
				}
			}
			
			if (columnModel.visible)
				columnModel.chosenColumn = columnIndex
		}

		onPositionChanged:	(mouse) =>
		{
			if(ribbonModel.dataMode && Boolean(mouse.modifiers & Qt.ShiftModifier))
				dataTableView.view.selectHover(rowIndex, columnIndex)
		}

	}

	// The visible selection highlight is drawn by the TableView delegate
	// wrapper (DataSetViewTable.qml) directly from the model role, so that it
	// follows scrolling and selection changes without any C++ bookkeeping.
	// This is only the geometry anchor for the mouse area and popup menu.
	Rectangle
	{
		id:				itemHighlight
		visible:			false
		z:					-1

		anchors
		{
			fill:			 parent
			topMargin:		-dataTableView.itemVerticalPadding
			leftMargin:		-dataTableView.itemHorizontalPadding
			rightMargin:	-dataTableView.itemHorizontalPadding
			bottomMargin:	-dataTableView.itemVerticalPadding
		}
	}

}

