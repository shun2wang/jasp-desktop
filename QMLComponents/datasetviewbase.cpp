#include "datasetviewbase.h"

#include "timers.h"
#include "log.h"
#include "preferencesmodelbase.h"
#include "jasptheme.h"
#include <QScreen>
#include <iostream>
#include <QGuiApplication>
#include <QClipboard>
#include <QQuickWindow>
#include "utils.h"
#include "emptyvalues.h"
#include <cmath>


DataSetViewTableModel::DataSetViewTableModel(DataSetViewBase *owner, Kind kind)
	: QIdentityProxyModel(owner), _owner(owner), _kind(kind)
{
	static const QStringList roles = {
		"itemText", "itemData", "itemEnabled", "itemTextEdit", "itemShadowText", "itemLabel", "itemValue",
		"itemActive", "itemEditable", "itemSelected", "itemFiltered", "itemInputType", "itemLines",
		"rowIndex", "columnIndex", "rowNumber", "headerText", "virtual", "columnTitle", "columnDescription", "columnIsComputed",
		"columnIsInvalidated", "computedColumnType", "columnIsFiltered", "columnError", "columnType",
		"isDynamic", "tableView", "dataviewer"
	};
	int role = Qt::UserRole + 10000;
	for(const QString &name : roles)
		_extraRoles.insert(role++, name.toUtf8());
}

// Both axes must track the source count for ALL kinds: QIdentityProxyModel
// forwards the source's structural-change notifications (rows/columns about
// to be inserted/removed, moved...) with the source's index ranges verbatim,
// and QAbstractItemModel asserts those ranges against this model's own
// rowCount/columnCount. A RowHeaders model that claims 1 column therefore
// asserts/crashes on every column removal. The rendered axis is limited to 1
// by the consuming TableView's rows/columns properties in DataSetViewTable.qml.
int DataSetViewTableModel::rowCount(const QModelIndex &parent) const
{
	if(parent.isValid() || !sourceModel())
		return 0;
	return sourceModel()->rowCount(parent);
}

int DataSetViewTableModel::columnCount(const QModelIndex &parent) const
{
	if(parent.isValid() || !sourceModel())
		return 0;
	return sourceModel()->columnCount(parent);
}

QVariant DataSetViewTableModel::data(const QModelIndex &index, int role) const
{
	if(!index.isValid() || !sourceModel())
		return {};
	if(role == Qt::DisplayRole)
		return _kind == Kind::Cells ? QIdentityProxyModel::data(index, role) : QVariant();
	if(!_extraRoles.contains(role))
		return QIdentityProxyModel::data(index, role);

	const QString roleName = QString::fromUtf8(_extraRoles.value(role));
	if(roleName == "isDynamic") return true;
	if(roleName == "rowIndex") return index.row();
	if(roleName == "columnIndex") return index.column();
	if(roleName == "tableView") return QVariant::fromValue<QObject *>(_owner->tableViewItem());
	if(roleName == "dataviewer") return QVariant::fromValue<QObject *>(_owner);

	if(_kind == Kind::Cells)
		return _owner->cellData(index.row(), index.column()).value(roleName);
	if(_kind == Kind::RowHeaders)
		return _owner->rowHeaderData(index.row()).value(roleName);
	return _owner->columnHeaderData(index.column()).value(roleName);
}

QHash<int, QByteArray> DataSetViewTableModel::roleNames() const
{
	QHash<int, QByteArray> roles = QIdentityProxyModel::roleNames();
	roles.insert(_extraRoles);
	return roles;
}

void DataSetViewTableModel::notifyDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QVector<int> &roles)
{
	if(!sourceModel() || !topLeft.isValid() || !bottomRight.isValid())
		return;

	static const QHash<QByteArray, QByteArray> roleAliases = {
		{"label", "itemLabel"}, {"value", "itemValue"}, {"shadowDisplay", "itemShadowText"},
		{"noSepaDisplay", "itemTextEdit"}, {"selected", "itemSelected"}, {"filter", "itemFiltered"},
		{"itemInputValue", "itemInputType"}, {"display", "itemText"}
	};
	const QHash<int, QByteArray> sourceRoles = sourceModel()->roleNames();
	QVector<int> translatedRoles;
	const QVector<int> sourceChanges = roles.isEmpty() ? sourceRoles.keys() : roles;
	for(int role : sourceChanges)
	{
		const QByteArray name = role == Qt::DisplayRole ? QByteArray("display") : sourceRoles.value(role);
		const int translatedRole = _extraRoles.key(roleAliases.value(name, name), -1);
		if(translatedRole >= 0 && !translatedRoles.contains(translatedRole))
			translatedRoles.append(translatedRole);
	}

	// QIdentityProxyModel already forwards the source roles. Add only the
	// historical aliases that cannot be represented by the source role list.
	if(!translatedRoles.isEmpty())
		emit dataChanged(index(topLeft.row(), _kind == Kind::RowHeaders ? 0 : topLeft.column()),
						 index(bottomRight.row(), _kind == Kind::RowHeaders ? 0 : bottomRight.column()), translatedRoles);
}

void DataSetViewTableModel::notifyHeaderDataChanged(Qt::Orientation orientation, int first, int last)
{
	if(!sourceModel() || first < 0 || last < first)
		return;

	if(orientation == Qt::Vertical && _kind == Kind::RowHeaders)
		emit dataChanged(index(first, 0), index(last, 0));
	else if(orientation == Qt::Horizontal && _kind == Kind::ColumnHeaders)
		emit dataChanged(index(0, first), index(0, last));
}

DataSetViewBase::DataSetViewBase(QQuickItem *parent)
	: QQuickItem (parent)
	, _cellModel(new DataSetViewTableModel(this, DataSetViewTableModel::Kind::Cells))
	, _rowHeaderModel(new DataSetViewTableModel(this, DataSetViewTableModel::Kind::RowHeaders))
	, _columnHeaderModel(new DataSetViewTableModel(this, DataSetViewTableModel::Kind::ColumnHeaders))
	, _selectionModel(new QItemSelectionModel(nullptr, this))
{
	//setFlag(QQuickItem::ItemIsFocusScope);

	_delayViewportChangedTimer = new QTimer(this);
	_delayViewportChangedTimer->setInterval(0);
	_delayViewportChangedTimer->setSingleShot(true);
	
	_resetLessTimer = new QTimer(this);
	_resetLessTimer->setInterval(100);
	_resetLessTimer->setSingleShot(true);
	
	

	connect(this,						&DataSetViewBase::parentChanged,					this, &DataSetViewBase::myParentChanged);
	
	connect(this,						&DataSetViewBase::viewportXChanged,					this, &DataSetViewBase::viewportChangedDelayed);
	connect(this,						&DataSetViewBase::viewportYChanged,					this, &DataSetViewBase::viewportChangedDelayed);
	connect(this,						&DataSetViewBase::viewportWChanged,					this, &DataSetViewBase::viewportChangedDelayed);
	connect(this,						&DataSetViewBase::viewportHChanged,					this, &DataSetViewBase::viewportChangedDelayed);
	connect(_delayViewportChangedTimer, &QTimer::timeout,									this, &DataSetViewBase::viewportChanged);
	connect(_resetLessTimer,			&QTimer::timeout,									this, &DataSetViewBase::modelWasReset);

	connect(this,						&DataSetViewBase::itemDelegateChanged,				this, &DataSetViewBase::reloadTextItems);
	connect(this,						&DataSetViewBase::rowNumberDelegateChanged,			this, &DataSetViewBase::reloadRowNumbers);
	connect(this,						&DataSetViewBase::columnHeaderDelegateChanged,		this, &DataSetViewBase::reloadColumnHeaders);

	connect(this,						&DataSetViewBase::itemHorizontalPaddingChanged,		this, &DataSetViewBase::calculateCellSizes);
	connect(this,						&DataSetViewBase::itemVerticalPaddingChanged,		this, &DataSetViewBase::calculateCellSizes);
	connect(this,						&DataSetViewBase::extraColumnItemChanged,			this, &DataSetViewBase::calculateCellSizes);

	connect(this,						&DataSetViewBase::itemSizeChanged,					this, &DataSetViewBase::reloadTextItems);
	connect(this,						&DataSetViewBase::itemSizeChanged,					this, &DataSetViewBase::reloadRowNumbers);
	connect(this,						&DataSetViewBase::itemSizeChanged,					this, &DataSetViewBase::reloadColumnHeaders);

	connect(this,						&DataSetViewBase::editingChanged,					this, &DataSetViewBase::updateDelegateData);
	connect(this,						&DataSetViewBase::editCoordinatesChanged,			this, &DataSetViewBase::updateDelegateData);
	
	connect(this,						&DataSetViewBase::selectionChanged,					this, &DataSetViewBase::selectionMinChanged);
	connect(this,						&DataSetViewBase::selectionChanged,					this, &DataSetViewBase::selectionMaxChanged);
	
	connect(PreferencesModelBase::preferences(), &PreferencesModelBase::uiScaleChanged,		this, &DataSetViewBase::resetItems,				Qt::QueuedConnection);
	connect(PreferencesModelBase::preferences(), &PreferencesModelBase::interfaceFontChanged,this, &DataSetViewBase::resetItems,			Qt::QueuedConnection);

	connect(_selectionModel,			&QItemSelectionModel::selectionChanged,				this, &DataSetViewBase::selectionDataChanged);
	connect(_selectionModel,			&QItemSelectionModel::selectionChanged,				this, &DataSetViewBase::selectionChanged);
	
	connect(this,						&QQuickItem::visibleChanged,						this, [this](){ if(isVisible()) modelWasReset(); }, Qt::QueuedConnection);

	setZ(10);
	
}

bool DataSetViewBase::event(QEvent *event)
{
	if(event->type() == QEvent::Polish)
		initializeTableView();
	return QQuickItem::event(event);
}

void DataSetViewBase::componentComplete()
{
	QQuickItem::componentComplete();
	initializeTableView();
}

void DataSetViewBase::initializeTableView()
{
	if(_tableView)
		return;
	if(!qmlEngine(this))
	{
		return;
	}

	QQmlComponent component(qmlEngine(this), QUrl("qrc:/jasp-stats.org/imports/JASP/Controls/components/JASP/Controls/DataSetViewTable.qml"));
	if(component.isError())
	{
		Log::log() << "Cannot load TableView renderer: " << component.errorString() << std::endl;
		return;
	}
	QQmlContext *context = new QQmlContext(qmlContext(this), this);
	context->setContextProperty("dataSetViewController", this);
	_tableView = qobject_cast<QQuickItem*>(component.create(context));
	if(!_tableView)
	{
		Log::log() << "Cannot create TableView renderer: " << component.errorString() << std::endl;
		delete context;
		return;
	}
	iAmParent(_tableView);
	_tableView->setZ(-5);
	auto updateGeometry = [this]()
	{
		if(_tableView)
		{
			_tableView->setPosition(QPointF(_viewportX, _viewportY));
			_tableView->setSize(QSizeF(std::max(0.0, _viewportW), std::max(0.0, _viewportH)));
		}
	};
	connect(this, &DataSetViewBase::viewportXChanged, this, updateGeometry);
	connect(this, &DataSetViewBase::viewportYChanged, this, updateGeometry);
	connect(this, &DataSetViewBase::viewportWChanged, this, updateGeometry);
	connect(this, &DataSetViewBase::viewportHChanged, this, updateGeometry);
	updateGeometry();
}

void DataSetViewBase::reloadTableView()
{
	if(!_tableView)
		return;
	QMetaObject::invokeMethod(_tableView, "synchronize", Qt::QueuedConnection);
}

void DataSetViewBase::setModel(QAbstractItemModel * model)
{
	if(_model == model)
		return;
	if(_model)
		disconnect(_model, nullptr, this, nullptr);
	clearEdit();
	_model = model;
	_cellModel->setSourceModel(model);
	_rowHeaderModel->setSourceModel(model);
	_columnHeaderModel->setSourceModel(model);

	if (model)
	{
		setRolenames();
		connect(model,				&QAbstractItemModel::modelReset,				this, &DataSetViewBase::modelWasReset				);
		connect(model,				&QAbstractItemModel::dataChanged,				this, &DataSetViewBase::modelDataChanged			);
		connect(model,				&QAbstractItemModel::headerDataChanged,			this, &DataSetViewBase::modelHeaderDataChanged		);
		connect(model,				&QAbstractItemModel::modelAboutToBeReset,		this, &DataSetViewBase::modelAboutToBeReset			);

		connect(model,				&QAbstractItemModel::columnsAboutToBeInserted,	this, &DataSetViewBase::columnsAboutToBeInserted	);
		connect(model,				&QAbstractItemModel::columnsAboutToBeRemoved,	this, &DataSetViewBase::columnsAboutToBeRemoved		);
		connect(model,				&QAbstractItemModel::rowsAboutToBeInserted,		this, &DataSetViewBase::rowsAboutToBeInserted		);
		connect(model,				&QAbstractItemModel::rowsAboutToBeRemoved,		this, &DataSetViewBase::rowsAboutToBeRemoved		);
		connect(model,				&QAbstractItemModel::columnsInserted,			this, &DataSetViewBase::columnsInserted,			Qt::QueuedConnection);
		connect(model,				&QAbstractItemModel::columnsRemoved,			this, &DataSetViewBase::columnsRemoved,				Qt::QueuedConnection);
		connect(model,				&QAbstractItemModel::rowsInserted,				this, &DataSetViewBase::rowsInserted,				Qt::QueuedConnection);
		connect(model,				&QAbstractItemModel::rowsRemoved,				this, &DataSetViewBase::rowsRemoved,				Qt::QueuedConnection);
		
		
		_selectionModel->setModel(model);
		emit selectionModelChanged(); //Or maybe it hasn't?
		
		setRowNumberWidth(getRowHeaderSize().width());

		//recalculateCellSizes = true;
		calculateCellSizes();
		update();

		emit modelChanged();
	}
	else
	{
		_selectionModel->setModel(nullptr);
		_cellSizes.clear();
		_dataColsMaxWidth.clear();
		_colXPositions.clear();
		setWidth(0);
		setHeight(0);
		emit modelChanged();
	}
}

void DataSetViewBase::setRolenames()
{
	_roleNameToRole.clear();

	auto roleNames = _model->roleNames();

	for(const auto & rn : roleNames.keys())
		_roleNameToRole[roleNames[rn].toStdString()] = rn;
}

int DataSetViewBase::getRole(const std::string &roleName) const
{
	auto it = _roleNameToRole.find(roleName);
	if (it == _roleNameToRole.end())
		return -1;
	else
		return it->second;
}


QVariantMap DataSetViewBase::cellData(int row, int column) const
{
	QVariantMap result;
	if(!_model)
		return result;
	const QModelIndex index = _model->index(row, column);
	if(!index.isValid())
	{
		static int invalidWarned = 0;
		if(invalidWarned++ < 5)
		return result;
	}
	const auto flags = _model->flags(index);
	if(_model->flags(index) & Qt::ItemIsEditable || _storedDisplayText.count(row) == 0 || _storedDisplayText.find(row)->second.count(column) == 0)
		const_cast<DataSetViewBase*>(this)->_storedDisplayText[row][column] = _model->data(index, Qt::DisplayRole).toString();
	result.insert("itemText", _storedDisplayText.find(row)->second.find(column)->second);
	result.insert("itemData", _model->data(index));
	result.insert("itemEnabled", bool(flags & Qt::ItemIsEnabled));
	result.insert("itemTextEdit", _model->data(index, getRole("noSepaDisplay")));
	result.insert("itemShadowText", _model->data(index, getRole("shadowDisplay")));
	result.insert("itemLabel", _model->data(index, getRole("label")));
	result.insert("itemValue", _model->data(index, getRole("value")));
	result.insert("itemActive", const_cast<DataSetViewBase*>(this)->isFiltered(row, column));
	result.insert("itemEditable", bool(flags & Qt::ItemIsEditable));
	const QVariant selected = _model->data(index, getRole("selected"));
	result.insert("itemSelected", selected.isValid() && selected.userType() != QMetaType::UnknownType ? selected.toBool() : _selectionModel->isSelected(index));
	result.insert("itemFiltered", _model->data(index, getRole("filter")));
	result.insert("itemInputType", _model->data(index, getRole("itemInputValue")));
	result.insert("itemLines", _model->data(index, getRole("lines")));
	return result;
}

QVariantMap DataSetViewBase::rowHeaderData(int row) const
{
	if(!_model)
		return {};
	return {
		{"rowNumber", _model->headerData(row, Qt::Vertical, Qt::DisplayRole)},
		{"virtual", const_cast<DataSetViewBase*>(this)->isRowVirtual(row)},
		{"headerText", _model->headerData(row, Qt::Vertical).toString()},
		{"itemLines", 15}
	};
}

QVariantMap DataSetViewBase::columnHeaderData(int column) const
{
	if(!_model)
		return {};
	return {
		{"headerText", _model->headerData(column, Qt::Horizontal).toString()},
		{"columnTitle", _model->headerData(column, Qt::Horizontal, getRole("title"))},
		{"columnDescription", _model->headerData(column, Qt::Horizontal, getRole("description"))},
		{"columnIsComputed", _model->headerData(column, Qt::Horizontal, getRole("columnIsComputed")).toBool()},
		{"columnIsInvalidated", _model->headerData(column, Qt::Horizontal, getRole("computedColumnIsInvalidated")).toBool()},
		{"computedColumnType", _model->headerData(column, Qt::Horizontal, getRole("computedColumnType")).toInt()},
		{"columnIsFiltered", _model->headerData(column, Qt::Horizontal, getRole("filter")).toBool()},
		{"columnError", _model->headerData(column, Qt::Horizontal, getRole("computedColumnError")).toString()},
		{"columnType", _model->headerData(column, Qt::Horizontal, getRole("columnType")).toInt()},
		{"virtual", const_cast<DataSetViewBase*>(this)->isColumnVirtual(column)},
		{"itemLines", 15}
	};
}

QQuickItem *DataSetViewBase::cellItem(int row, int column) const
{
	return _cellItems.key(QPoint(column, row), nullptr);
}

QQuickItem * DataSetViewBase::createDelegate(const QString &kind, QQmlComponent *component, QQuickItem *parent, int row, int column)
{
	if(!component || !parent || !_model)
		return nullptr;

	// Parent the delegate context on the QML file that instantiated this view
	// (DataTableView.qml), so ids like dataTableView/ribbonModel/columnModel
	// resolve exactly like they did when delegates were created there directly.
	QQmlContext *context = new QQmlContext(qmlContext(this), this);
	if(kind == "cell")
		setStyleDataItem(context, isFiltered(row, column), column, row);
	else if(kind == "row")
		setStyleDataRowNumber(context, row);
	else if(kind == "column")
		setStyleDataColumnHeader(context, column);
	else
	{
		delete context;
		return nullptr;
	}

	context->setContextProperty("rowIndex", row);
	context->setContextProperty("columnIndex", column);
	context->setContextProperty("isDynamic", true);
	context->setContextProperty("tableView", _tableViewItem);
	context->setContextProperty("dataviewer", this);

	QQmlIncubator incubator(QQmlIncubator::Synchronous);
	component->create(incubator, context);
	QQuickItem *	item = qobject_cast<QQuickItem *>(incubator.object());
	if(incubator.isError() || !item)
	{
		for(const QQmlError &error : incubator.errors())
			Log::log() << "Delegate incubation error: " << error.toString().toStdString() << std::endl;
		Log::log() << "Cannot create " << kind.toStdString() << " delegate at row " << row << ", column " << column << std::endl;
		delete incubator.object();
		delete context;
		return nullptr;
	}

	item->setParentItem(parent);
	setDelegateGeometry(kind, item, row, column);
	_delegateContexts.insert(item, context);
	if(kind == "cell")
		_cellItems.insert(item, QPoint(column, row));
	else if(kind == "row")
		_rowNumberItems.insert(row, item);
	else
		_columnHeaderItems.insert(column, item);


	connect(item, &QObject::destroyed, this, [this, item]()
	{
		_cellItems.remove(item);
		for(auto it = _rowNumberItems.begin(); it != _rowNumberItems.end();)
			it.value() == item ? it = _rowNumberItems.erase(it) : ++it;
		for(auto it = _columnHeaderItems.begin(); it != _columnHeaderItems.end();)
			it.value() == item ? it = _columnHeaderItems.erase(it) : ++it;
		delete _delegateContexts.take(item);
	});

	return item;
}

void DataSetViewBase::destroyDelegate(const QString &kind, QQuickItem *item)
{
	if(!item)
		return;

	QQmlContext *context = _delegateContexts.take(item);
	disconnect(item, nullptr, this, nullptr);
	_cellItems.remove(item);
	for(auto it = _rowNumberItems.begin(); it != _rowNumberItems.end();)
		it.value() == item ? it = _rowNumberItems.erase(it) : ++it;
	for(auto it = _columnHeaderItems.begin(); it != _columnHeaderItems.end();)
		it.value() == item ? it = _columnHeaderItems.erase(it) : ++it;

	item->setFocus(false);
	item->setVisible(false);
	delete item;
	delete context;
}

void DataSetViewBase::setDelegateGeometry(const QString &kind, QQuickItem *item, int row, int column)
{
	if(!item)
		return;

	item->setX(kind == "cell" ? _itemHorizontalPadding : 0);
	item->setY(kind == "cell" ? _itemVerticalPadding : 0);
	item->setWidth(kind == "row" ? _rowNumberMaxWidth : std::max(0.0, columnWidth(column) - (kind == "cell" ? 2 * _itemHorizontalPadding : 0)));
	item->setHeight(std::max(0.0, _dataRowsMaxHeight - (kind == "cell" ? 2 * _itemVerticalPadding : 0)));
	item->setVisible(kind != "cell" || !_editing || _prevEditRow != row || _prevEditCol != column);
}

void DataSetViewBase::updateDelegateData()
{
	for(auto it = _delegateContexts.constBegin(); it != _delegateContexts.constEnd(); ++it)
	{
		QQuickItem *item = it.key();
		QQmlContext *context = it.value();
		const QPoint cell = _cellItems.value(item, QPoint(-1, -1));
		if(cell.x() >= 0)
		{
			setStyleDataItem(context, isFiltered(cell.y(), cell.x()), cell.x(), cell.y());
			setDelegateGeometry("cell", item, cell.y(), cell.x());
			continue;
		}

		const int row = _rowNumberItems.key(item, -1);
		if(row >= 0)
		{
			setStyleDataRowNumber(context, row);
			setDelegateGeometry("row", item, row, 0);
			continue;
		}

		const int column = _columnHeaderItems.key(item, -1);
		if(column >= 0)
		{
			setStyleDataColumnHeader(context, column);
			setDelegateGeometry("column", item, 0, column);
		}
	}
}

QSizeF DataSetViewBase::getColumnSize(int col)
{
	JASPTIMER_SCOPE(DataSetViewBase::getColumnSize);
	
	QVariant maxColStringVar = _model->headerData(col, Qt::Orientation::Horizontal, getRole("maxColString"));
	
	QSizeF colSize;
	
	if(!maxColStringVar.isNull())
		colSize = getTextSize(maxColStringVar.toString());
	else
	{
		JASPTIMER_SCOPE(DataSetViewBase::getColumnSize fallback);
		
		QVariant columnWidthFallbackVar = _model->headerData(col, Qt::Orientation::Horizontal, getRole("columnWidthFallback"));

		colSize = getTextSize("??????");

		if(!columnWidthFallbackVar.isNull())
			colSize.setWidth(columnWidthFallbackVar.toFloat() - itemHorizontalPadding() * 2);
	}
	
	return  QSizeF(_maxColWidth <= 0 ? colSize.width() : std::min(colSize.width(), double(_maxColWidth)), colSize.height());
}

QSizeF DataSetViewBase::getRowHeaderSize()
{
	JASPTIMER_SCOPE(DataSetViewBase::getRowHeaderSize);
	
	QString text = _model->headerData(0, Qt::Orientation::Vertical, getRole("maxRowHeaderString")).toString();

	return getTextSize(text);
}

void DataSetViewBase::clearCaches()
{
	// TableView owns the wrapper items. Force a layout so they are recreated
	// with a fresh font context, without orphaning live delegates in the maps.
	reloadTableView();
}

void DataSetViewBase::modelDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QVector<int> &roles)
{
	if(!isVisible() || !_model || !topLeft.isValid() || !bottomRight.isValid() || topLeft.column() < 0 || topLeft.column() >= _model->columnCount() || topLeft.column() >= int(_cellSizes.size()))
		return;

	_cellModel->notifyDataChanged(topLeft, bottomRight, roles);
	updateDelegateData();

	const int	colMin = std::max(0,						topLeft.column()),
				colMax = std::min(_model->columnCount() - 1,	bottomRight.column()),
				rowMin = std::max(0,						topLeft.row()),
				rowMax = std::min(_model->rowCount() - 1,	bottomRight.row());

	QSizeF calcSize = getColumnSize(colMin);

	if (_cacheItems || int(_cellSizes[size_t(colMin)].width() * 10) != int(calcSize.width() * 10)) //If we cache items we are not expecting the user to make regular manual changes to the data, so if something changes we can do a reset. Otherwise we are in TableView and we do it only when the column size changes.
		calculateCellSizes();

	if (editing() && (topLeft == bottomRight)) // In case of Undo/Redo set the focus on the correspondig cell
		positionEditItem(topLeft.row(), topLeft.column());


	//The following else would be good but it doesnt seem to work on mac for some reason. It does work on linux though
	/*else
		for(size_t row=topLeft.row(); row<=bottomRight.row(); row++)
			for(size_t col=topLeft.column(); col<=bottomRight.column(); col++)
			{
				storeTextItem(row, col);
				createTextItem(row, col);
			}*/

}

void DataSetViewBase::modelHeaderDataChanged(Qt::Orientation orientation, int first, int last)
{
	if(isVisible())
	{
		_rowHeaderModel->notifyHeaderDataChanged(orientation, first, last);
		_columnHeaderModel->notifyHeaderDataChanged(orientation, first, last);
		calculateCellSizes();
	}
}

void DataSetViewBase::modelAboutToBeReset()
{
	_storedDisplayText.clear();
}

void DataSetViewBase::modelWasReset()
{
	if(isVisible())
		calculateCellSizes();

	
	/*QModelIndex startIndex	= _model->index(_selectionStart.y(),	_selectionStart.x()),
				endIndex	= _model->index(_selectionEnd.y(),		_selectionEnd.x());
	
	if(!endIndex.isValid())
	{
		if(!startIndex.isValid())
			_selectionModel->clear();
		else
			_selectionModel->select(startIndex, QItemSelectionModel::ClearAndSelect);
	}
	else
		_selectionModel->select(QItemSelection(startIndex, endIndex), QItemSelectionModel::ClearAndSelect);*/
}

void DataSetViewBase::resetItems()
{
	calculateCellSizesAndClear(true); //We clear storage because otherwise the wrong font/scaling gets remembered by the item
}

void DataSetViewBase::calculateCellSizesAndClear(bool clearStorage)
{
	JASPTIMER_SCOPE(DataSetViewBase::calculateCellSizes);

	
	_cellSizes.clear();
	_dataColsMaxWidth.clear();
	_storedDisplayText.clear();

	if(clearStorage)
		clearCaches();
	
	if(_model == nullptr) return;

	_cellSizes.resize(_model->columnCount());
	_colXPositions.resize(_model->columnCount());

	for(int col=0; col<_model->columnCount(); col++)
		_cellSizes[col] = getColumnSize(col);

	// Snap the total row height (content + padding) to whole device pixels: a
	// fractional height makes the 1px grid lines land between device pixels,
	// which the rasterizer smears into 2px lines every other row (esp. visible
	// on Windows at 125%/150%), and it desyncs the C++-positioned editor/highlight
	// from the TableView-positioned cells.
	qreal devicePixelRatio = window() && window()->screen() ? window()->devicePixelRatio() : 1.0;
	qreal snappedRowHeight = std::round((_cellSizes.empty() ? 0 : _cellSizes[0].height()) * devicePixelRatio) / devicePixelRatio + _itemVerticalPadding * 2;
	snappedRowHeight = std::round(snappedRowHeight * devicePixelRatio) / devicePixelRatio;
	if(snappedRowHeight <= 0)
		snappedRowHeight = 1;

	_dataColsMaxWidth.resize(_model->columnCount());

	for(int col=0; col<_model->columnCount(); col++)
		_dataColsMaxWidth[col] = std::round((_cellSizes[col].width() + _itemHorizontalPadding * 2) * devicePixelRatio) / devicePixelRatio;

	setHeaderHeight(_model->columnCount() == 0 ? 0 : snappedRowHeight);
	setRowNumberWidth(std::round(getRowHeaderSize().width() * devicePixelRatio) / devicePixelRatio);

	
	float w = _rowNumberMaxWidth;
	for(int col=0; col<_model->columnCount(); col++)
		w += _dataColsMaxWidth[col];

	float x = _rowNumberMaxWidth;

	for(int col=0; col<_model->columnCount(); col++)
	{
		_colXPositions[col] = x;
		x += _dataColsMaxWidth[col];
	}

	_dataWidth = w;

	qreal	newWidth	= (_extraColumnItem != nullptr && !expandDataSet() ? _dataRowsMaxHeight + 1 : 0 ) + _dataWidth,
			newHeight	= _dataRowsMaxHeight * (_model->rowCount() + 1);

	//Log::log() << "Settings WxH: " << newWidth << "X" << newHeight << std::endl;

	setWidth(	newWidth);
	setHeight(	newHeight);
	_recalculateCellSizes = false;

	//emit itemSizeChanged(); //This calls reloadTextItems, reloadRowNumbers and reloadColumnHeaders and those all call viewPortChanged. Which recreates them all every time if necessary... Nobody else seems to emit this signal anywhere so I dont see the point. Ill replace it with viewPortChanged

	reloadTableView();
	updateDelegateData();
	viewportChangedDelayed();
}

void DataSetViewBase::viewportChangedDelayed()
{
    //_delayViewportChangedTimer->start();
    viewportChanged();
}

void DataSetViewBase::viewportChanged()
{
	if(_model == nullptr || _viewportX != _viewportX || _viewportY != _viewportY || _viewportW != _viewportW || _viewportH != _viewportH ) //only possible if they are NaN
		return;

	if(_dataColsMaxWidth.size() != _model->columnCount())
		return;

	JASPTIMER_RESUME(DataSetViewBase::viewportChanged);

#ifdef DATASETVIEW_DEBUG_VIEWPORT
	Log::log() << "viewportChanged!\n" <<std::flush;
#endif

	determineCurrentViewPortIndices();
	
	if(_viewportW >= 0 && _viewportH >= 0)
	{
		buildNewLinesAndCreateNewItems();
		update();
	}

	_previousViewportColMin = _currentViewportColMin;
	_previousViewportColMax = _currentViewportColMax;
	_previousViewportRowMin = _currentViewportRowMin;
	_previousViewportRowMax = _currentViewportRowMax;

	JASPTIMER_STOP(DataSetViewBase::viewportChanged);
}


void DataSetViewBase::determineCurrentViewPortIndices()
{
	JASPTIMER_RESUME(DataSetViewBase::determineCurrentViewPortIndices);
	QVector2D leftTop(_viewportX, _viewportY);
	QVector2D viewSize(_viewportW, _viewportH);
	QVector2D rightBottom(leftTop + viewSize);

	_currentViewportColMax = -1;
	_currentViewportColMin = -1;

	float cumulative = 0;
	for(int col=0; col<_model->columnCount() && _currentViewportColMax == -1; col++)
	{
		if(_currentViewportColMax == -1 && cumulative > rightBottom.x())						_currentViewportColMax = col;

		float prevCum = cumulative;
		cumulative += _dataColsMaxWidth[col];

		if(_currentViewportColMin == -1 && cumulative > leftTop.x() && prevCum < leftTop.x())	_currentViewportColMin = col;
	}

	if(_currentViewportColMax == -1)
		_currentViewportColMax = _model->columnCount();

    _currentViewportColMin = std::max(0, std::min(_model->columnCount(),	_currentViewportColMin							- _viewportMargin));
    _currentViewportColMax = std::max(0, std::min(_model->columnCount(),	_currentViewportColMax							+ _viewportMargin));

	_currentViewportRowMin = _dataRowsMaxHeight == 0 ? 0 : std::max(0, std::min(_model->rowCount(),		qRound(leftTop.y()		/ _dataRowsMaxHeight)	- (1 + _viewportMargin)));
	_currentViewportRowMax = _dataRowsMaxHeight == 0 ? 0 : std::max(0, std::min(_model->rowCount(),		qRound(rightBottom.y()	/ _dataRowsMaxHeight)	+ (1 + _viewportMargin)));

#ifdef DATASETVIEW_DEBUG_VIEWPORT
	Log::log() << "viewport X: " << _viewportX << " Y: " << _viewportY << " W: " << _viewportW << " H: " << _viewportH <<  std::endl;
	Log::log() << "_previousViewport\tColMin: " << _previousViewportColMin << "\tColMax: " << _previousViewportColMax << "\tRowMin: " << _previousViewportRowMin << "\tRowMax: " << _previousViewportRowMax << "\n";
	Log::log() << "_currentViewport\tColMin: "  << _currentViewportColMin  << "\tColMax: " << _currentViewportColMax  << "\tRowMin: " << _currentViewportRowMin  << "\tRowMax: " << _currentViewportRowMax  << "\n" << std::flush;
#endif
	JASPTIMER_STOP(DataSetViewBase::determineCurrentViewPortIndices);
}

QSizeF DataSetViewBase::getTextSize(const QString & text) const
{
	return JaspTheme::fontMetrics().size(Qt::TextSingleLine, text);
}

void DataSetViewBase::buildNewLinesAndCreateNewItems()
{
	JASPTIMER_RESUME(DataSetViewBase::buildNewLinesAndCreateNewItems);

	if(_currentViewportColMax == -1 ||  _currentViewportColMin == -1 || _currentViewportRowMax == -1 || _currentViewportRowMin == -1)
		return;

	// Grid lines, delegates, corner and editor are all rendered by the QML
	// TableView layer now (DataSetViewTable.qml draws cell borders).

	createleftTopCorner();
	updateExtraColumnItem();

	if(editing())
		positionEditItem(_prevEditRow, _prevEditCol);
	else
		destroyEditItem();

	JASPTIMER_STOP(DataSetViewBase::buildNewLinesAndCreateNewItems);
}

void DataSetViewBase::iAmParent(QQuickItem * item)
{
	if(item->thread() != QThread::currentThread())
		item->moveToThread(QThread::currentThread());
	item->setParent(this);
	item->setParentItem(this);
}

QQuickItem * DataSetViewBase::createleftTopCorner()
{
	//Log::log() << "createleftTopCorner() called!\n" << std::flush;
	if(_leftTopItem == nullptr)
	{

		if(_leftTopCornerDelegate == nullptr)
		{
			_leftTopCornerDelegate = new QQmlComponent(qmlEngine(this));
			_leftTopCornerDelegate->setData("import QtQuick \nItem {}", QUrl());
		}

		QQmlIncubator localIncubator(QQmlIncubator::Synchronous);
		_leftTopCornerDelegate->create(localIncubator);
		_leftTopItem = qobject_cast<QQuickItem*>(localIncubator.object());

		iAmParent(_leftTopItem);


		_leftTopItem->setVisible(true);
	}

	_leftTopItem->setHeight(_dataRowsMaxHeight - 2);
	_leftTopItem->setWidth(_rowNumberMaxWidth  - 2);
	_leftTopItem->setX(_viewportX + 1);
	_leftTopItem->setY(_viewportY + 1);
	_leftTopItem->setZ(-3);

	return _leftTopItem;
}

void DataSetViewBase::updateExtraColumnItem()
{
	//Log::log() << "createleftTopCorner() called!\n" << std::flush;
	if(!_extraColumnItem)
		return;

	_extraColumnItem->setHeight(_dataRowsMaxHeight - 1);
	_extraColumnItem->setX(_viewportX + _viewportW - extraColumnWidth());
	_extraColumnItem->setY(1 + _viewportY);

	connect(_extraColumnItem, &QQuickItem::widthChanged, this, &DataSetViewBase::setExtraColumnX, Qt::UniqueConnection);
}

void DataSetViewBase::destroyEditItem(bool createItem)
{
	if(!_editItemContextual)
	{
		_prevEditRow = -1;
		_prevEditCol = -1;
		return;
	}

	Log::log() << "Destroying old edit item (row=" << _prevEditRow << ", col=" << _prevEditCol << ") and context" << std::endl;

	_editItemContextual->item		->setVisible(false);
	_editItemContextual->item		->deleteLater();
	_editItemContextual->item		= nullptr;

	_editItemContextual->context	->deleteLater();
	_editItemContextual->context	= nullptr;

	delete _editItemContextual;
	_editItemContextual				= nullptr;

	if(createItem && _prevEditRow >= 0 && _prevEditCol >= 0)
	{
		if(QQuickItem *item = cellItem(_prevEditRow, _prevEditCol))
			item->setVisible(true);
	}
	else
		Log::log() << "Not creating text item" << std::endl;


	_prevEditRow = -1;
	_prevEditCol = -1;
}

void DataSetViewBase::positionEditItem(int row, int col)
{
	if (row == -1 || col == -1)
		return;

	if(!_editDelegate)
	{
		_editDelegate = new QQmlComponent(qmlEngine(this));

		_editDelegate->setData(
"import QtQuick"																					"\n"
"TextInput { text: itemText; color: itemActive ? 'black' : 'grey'; verticalAlignment: Text.AlignVCenter; \n"
" onEditingFinished:					 dataview.commitEdit(rowIndex, columnIndex, text); "									"\n"
"}", QUrl());

		emit editDelegateChanged(_editDelegate);
	}

	bool			active		= isFiltered(row, col);

	if(_editItemContextual && !(_prevEditRow == row && _prevEditCol == col)) //remove previous edit item to avoid old values or broken bindings messing everything up. But only if it is in a different place than where we're at
		destroyEditItem();


	if(!_editItemContextual)
	{
		_editItemContextual = new ItemContextualized(setStyleDataItem(nullptr, active, col, row, false));
		//Log::log() << "Edit item has          _storedDisplayText[" << row << "][" << col << "]: '" << _storedDisplayText[row][col] << "'" << std::endl;

		//forceActiveFocus();

		// The cell delegate remains owned by TableView and is hidden by C++ while editing.
		_prevEditRow = row; //Keep the coordinates for the editor and restoration
		_prevEditCol = col;
		
		emit editCoordinatesChanged();

		QQmlIncubator localIncubator(QQmlIncubator::Synchronous);
		_editDelegate->create(localIncubator, _editItemContextual->context);

		if(localIncubator.isError())
			throw std::runtime_error("Something went wrong incubating an edit item delegate for tableview!");

		_editItemContextual->item = qobject_cast<QQuickItem*>(localIncubator.object());
		iAmParent(_editItemContextual->item);
	}
	else
	{
		//Log::log() << "repositioning current edit item (row=" << row << ", col=" << col << ")" << std::endl;
		setStyleDataItem(_editItemContextual->context, active, col, row, false);
		//Log::log() << "Edit item has          _storedDisplayText[" << row << "][" << col << "]: '" << _storedDisplayText[row][col] << "'" << std::endl;
	}

	positionItem(row, col, _editItemContextual->item); //Will set it visible
	//_editItemContextual->item->setFocus(true);
}

void DataSetViewBase::positionItem(int row, int col, QQuickItem *item)
{
	if(!item)
		return;

	//The view itself is a content item of the surrounding Flickable and scrolls
	//with it, so the editor is placed in content coordinates (no viewport
	//compensation), just like the old C++ text items were.
	item->setPosition(QPointF(columnX(col) + _itemHorizontalPadding,
							_dataRowsMaxHeight + row * _dataRowsMaxHeight + _itemVerticalPadding));
	item->setSize(QSizeF(std::max(0.0, columnWidth(col) - 2 * _itemHorizontalPadding),
					   std::max(0.0, _dataRowsMaxHeight - 2 * _itemVerticalPadding)));
	item->setZ(-4.5);
	item->setVisible(true);
	iAmParent(item);
}

void DataSetViewBase::setExtraColumnX()
{
	_extraColumnItem->setX(_viewportX + _viewportW - extraColumnWidth());
}


bool DataSetViewBase::isSelected(int row, int col)
{
	return _selectionModel->isSelected(_model->index(row, col));
}


bool DataSetViewBase::relaxForSelectScroll()
{
	JASPTIMER_SCOPE(DataSetViewBase::relaxForSelectScroll);
	int64_t curMs = Utils::currentMillis();

	//Log::log() << "_selectScrollMs = " << _selectScrollMs << ", curMs = " << curMs << std::endl;

	if(curMs < _selectScrollMs)
		return false;

	_selectScrollMs = curMs + 100;
	return true;
}


void DataSetViewBase::pollSelectScroll(int row, int col)
{
	if(!relaxForSelectScroll())
		return;


	const int b = 3;

	//Log::log() << "DataSetViewBase::pollSelectScroll row=" << mouseIndex.row() << "col=" << mouseIndex.column() << std::endl;


		int		minMouseCol = col == -1 ? _currentViewportColMin :	col - b,
			maxMouseCol = col == -1 ? _currentViewportColMax :	col + b,
			minMouseRow = row == -1 ? _currentViewportRowMin :	row - b,
			maxMouseRow = row == -1 ? _currentViewportRowMax :	row + b;

	bool	maybeBudgeLeft	= minMouseCol <= 	_currentViewportColMin + _viewportMargin,
			maybeBudgeRight	= maxMouseCol >		_currentViewportColMax - _viewportMargin,
			maybeBudgeUp	= minMouseRow < 	_currentViewportRowMin + _viewportMargin,
			maybeBudgeDown	= maxMouseRow >=	_currentViewportRowMax - _viewportMargin;

			maybeBudgeLeft	= maybeBudgeLeft	&& (_currentViewportColMin > 0						|| 0						<	col);
			maybeBudgeUp	= maybeBudgeUp		&& (_currentViewportRowMin > 0						|| 0						<	row);
			maybeBudgeRight	= maybeBudgeRight	&& (_currentViewportColMax < _model->columnCount()	|| _model->columnCount()	>	col);
			maybeBudgeDown	= maybeBudgeDown	&& (_currentViewportRowMax < _model->rowCount()		|| _model->rowCount()		>	row);

	if(maybeBudgeLeft)	emit selectionBudgesLeft	();
	if(maybeBudgeUp)	emit selectionBudgesUp		();
	if(maybeBudgeRight) emit selectionBudgesRight	();
	if(maybeBudgeDown)	emit selectionBudgesDown	();
}






bool DataSetViewBase::clipBoardPasteIsCells() const
{
	QString clipboardStr = QGuiApplication::clipboard()->text();

	if (_lastJaspCopyIntoClipboard != clipboardStr)
		return clipboardStr.contains("\n");
	
	return !(_lastJaspCopyValues.size() == 1 && _lastJaspCopyValues[0].size() == 1);
}


void DataSetViewBase::selectAll()
{
	JASPTIMER_SCOPE(DataSetViewBase::selectAll);
	clearEdit();
	if(!_model || _model->rowCount() == 0 || _model->columnCount() == 0)
	{
		_selectionModel->clearSelection();
		return;
	}
	_selectionModel->select(QItemSelection(_model->index(0, 0), _model->index(_model->rowCount()-1, _model->columnCount()-1)), QItemSelectionModel::Current | QItemSelectionModel::ClearAndSelect);
}

void DataSetViewBase::select(int row, int col, bool shiftPressed, bool ctrlCmdPressed)
{
	JASPTIMER_SCOPE(DataSetViewBase::select);
	if(!_model || _model->rowCount() == 0 || _model->columnCount() == 0)
	{
		return;
	}
	
	bool	wholeRow	= col < 0,
			wholeCol	= row < 0;
	
	if(wholeRow && wholeCol)
	{
		selectAll();
		return;
	}
	
					row = std::clamp(row, 0, _model->rowCount() - 1);
					col = std::clamp(col, 0, _model->columnCount() - 1);

	QModelIndex		tl	= _model->index(row,					col),
					br	= _model->index(row,					col);

	if(wholeRow)	br	= _model->index(row,						_model->columnCount()-1);
	if(wholeCol)	br	= _model->index(_model->rowCount()-1,	col);
	
	QItemSelection	selection(tl, br);
	
	QItemSelectionModel::SelectionFlags flags = QItemSelectionModel::Current | QItemSelectionModel::ClearAndSelect; //Current means we will update the stored selection, so we commit here
	
	if(ctrlCmdPressed)
	{
		QItemSelection old = _selectionModel->selection();
		if(!wholeRow && !wholeCol)	old.merge(selection, QItemSelectionModel::Toggle);
		else if(wholeRow) //If a whole row is "toggled" we first make it all selected if it isnt yet fully, otherwise we deselect
		{
			if(_selectionModel->isRowSelected(row))		old.merge(selection, QItemSelectionModel::Deselect);
			else										old.merge(selection, QItemSelectionModel::Select);
		}
		else if(wholeCol) //We do the same for the column
		{
			if(_selectionModel->isColumnSelected(col))		old.merge(selection, QItemSelectionModel::Deselect);
			else											old.merge(selection, QItemSelectionModel::Select);
		}
			
		selection = old;
	}
	else if(shiftPressed)
	{
		QPoint	minNewSelection = minQModelIndex(selection),
				maxNewSelection = maxQModelIndex(selection),
				minOldSelection = selectionMin(),
				maxOldSelection = selectionMax();
		
		if(minOldSelection != QPoint{-1, -1} && minNewSelection != QPoint{-1, -1})
			selection = QItemSelection(	_model->index(std::min(minNewSelection.y(), minOldSelection.y()), std::min(minNewSelection.x(), minOldSelection.x())), 
										_model->index(std::max(maxNewSelection.y(), maxOldSelection.y()), std::max(maxNewSelection.x(), maxOldSelection.x())));
	}
	
	//Reset edit if we are selecting things with shift or ctrl/cmd, or when the last clicked place is not the same as the editthing
	if(shiftPressed || ctrlCmdPressed || row != _prevEditRow || col != _prevEditCol)
		clearEdit();
	
	if(!wholeCol && !shiftPressed && !ctrlCmdPressed && ( row != _prevEditRow || col != _prevEditCol) )
		edit(row, col);

	
/* //Even if you reenable this we prob dont want this in release par accident
#ifdef JASP_DEBUG
	{	
		QStringList l;
		for(QModelIndex i : selection.indexes())
			l << QString("(r=%1, c=%2)").arg(i.row()).arg(i.column());
		
		Log::log()	<< "DataSetViewBase::select(row=" << row << ", col=" << col << ", shift=" << shiftPressed << ", control="<< ctrlCmdPressed << "):\n"
					<< QString("-\tModel size: r=%1, c=%2\n").arg(_model->columnCount(false)).arg(_model->rowCount(false))
					<< QString("-\ttl = (r=%1, c=%2)\n").arg(tl.row()).arg(tl.column()) 
					<< QString("-\tbr = (r=%1, c=%2)\n").arg(br.row()).arg(br.column()) 
					<< "-\tset = " << l.join(", ") << std::endl;
	}
#endif*/
	
	_selectionModel->select(selection, flags);
}

void DataSetViewBase::selectionClear()
{
	JASPTIMER_SCOPE(DataSetViewBase::selectClear);
	_selectionModel->clear();
}

void DataSetViewBase::selectHover(int row, int col)
{
	JASPTIMER_SCOPE(DataSetViewBase::selectHover);
	pollSelectScroll(row, col);
}

void DataSetViewBase::notifyItemSelected(const QModelIndex &topLeft, const QModelIndex &bottomRight)
{
	if(!_model || !topLeft.isValid() || !bottomRight.isValid())
		return;

	emit _cellModel->dataChanged(_cellModel->index(topLeft.row(), topLeft.column()),
								_cellModel->index(bottomRight.row(), bottomRight.column()),
								{DataSetViewTableModel::ItemSelectedRole});
}

void DataSetViewBase::selectionDataChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
	for(const QItemSelectionRange &range : deselected)
		notifyItemSelected(range.topLeft(), range.bottomRight());
	for(const QItemSelectionRange &range : selected)
		notifyItemSelected(range.topLeft(), range.bottomRight());
	updateDelegateData();
}

QPoint DataSetViewBase::selectionMin() const
{ 
	JASPTIMER_SCOPE(DataSetViewBase::selectionMin);
	return minQModelIndex(_selectionModel->selection());	
}

QPoint DataSetViewBase::selectionMax() const
{ 
	JASPTIMER_SCOPE(DataSetViewBase::selectionMax);
	return maxQModelIndex(_selectionModel->selection());
}

void DataSetViewBase::columnIndexSelectedApply(int columnIndex, std::function<void(intset columnIndexes)> applyThis)
{
	int		columnA			= selectionMin().x(),
			columnB			= selectionMax().x();
	bool	outSelection	= columnA == -1 || columnB == -1 || !(columnIndex >= columnA && columnIndex <= columnB);
	
	if(outSelection)
		columnA = columnB = columnIndex;
	
	intset ints;
	for	(columnIndex	= columnA; columnIndex <= columnB; columnIndex++)
		if(outSelection || _selectionModel->columnIntersectsSelection(columnIndex))
			ints.insert(columnIndex);
	
	applyThis(ints);
}

void DataSetViewBase::columnIndexSelectedApply(int columnIndex, std::function<void(int columnIndex)> applyThis)
{
	columnIndexSelectedApply(columnIndex, [&](intset ints)
	{
		for	(int anInt : ints)
			applyThis(anInt);
	});
}

void DataSetViewBase::columnsAboutToBeInserted(const QModelIndex & parent, int first, int last)
{
	//temp:
	modelAboutToBeReset();
}

void DataSetViewBase::columnsAboutToBeRemoved(const QModelIndex & parent, int first, int last)
{
	//temp:
	modelAboutToBeReset();
}

void DataSetViewBase::rowsAboutToBeInserted(const QModelIndex & parent, int first, int last)
{
	//temp:
	modelAboutToBeReset();
}

void DataSetViewBase::rowsAboutToBeRemoved(const QModelIndex & parent, int first, int last)
{
	//temp:
	modelAboutToBeReset();
}

void DataSetViewBase::columnsInserted(const QModelIndex & parent, int first, int last)
{
	_resetLessTimer->start();
}

void DataSetViewBase::columnsRemoved(const QModelIndex & parent, int first, int last)
{
	_resetLessTimer->start();
}

void DataSetViewBase::rowsInserted(const QModelIndex & parent, int first, int last)
{
	_resetLessTimer->start();
}

void DataSetViewBase::rowsRemoved(const QModelIndex & parent, int first, int last)
{
	_resetLessTimer->start();
}

void DataSetViewBase::setEditDelegate(QQmlComponent *editDelegate)
{
	if (_editDelegate == editDelegate)
		return;

	_editDelegate = editDelegate;
	emit editDelegateChanged(_editDelegate);
}



void DataSetViewBase::setEditing(bool editing)
{
	if (_editing == editing)
		return;

	_editing = editing;

	//Log::log() << "DataSetViewBase::setShiftSelectActive( " << (shiftSelectActive? "active!" : "inactive!") << " )" << std::endl;

	emit editingChanged(_editing);
}

void DataSetViewBase::clearEdit()
{
	if(editing())
	{
		commitLastEdit();
		destroyEditItem();
		setEditing(false);
		updateDelegateData(); //unhide the cell delegate that was being edited
	}
}


void DataSetViewBase::edit(int row, int col)
{
	if(!_model || row < 0 || col < 0 || row >= _model->rowCount() || col >= _model->columnCount())
		return;

	_selectionModel->setCurrentIndex(_model->index(row, col), QItemSelectionModel::Current);

	clearEdit();

	setEditing(true);
	updateDelegateData(); //hide the cell delegate that is being edited
	positionEditItem(row, col);
}

void DataSetViewBase::commitEdit(int row, int col, QVariant editedValue)
{
	if(!editing())
	{
		Log::log() << "commitEdit called while not editing..." << std::endl;
	}
	else
	{
		QModelIndex modelIndex = _model->index(row, col);
		QVariant oldValue = _model->data(modelIndex);

		Log::log() << "editing finished! old value: '" << oldValue.toString() << "'  and new value: '" << editedValue.toString() << "' (row=" << row << ", col=" << col << ")" << std::endl;

		if(oldValue.toString() != editedValue.toString())
			_model->setData(modelIndex, editedValue, 0);
	}
}

void DataSetViewBase::onDataModeChanged(bool dataMode)
{
	if(!dataMode && editing())
	{
		destroyEditItem();
		setEditing(false);
	}
}

void DataSetViewBase::commitLastEdit()
{
	Log::log() << "Commit last edit" << std::endl;
	if(_prevEditRow != -1 && _prevEditCol != -1 && _editItemContextual && _editItemContextual->item && _editItemContextual->item->property("text").isValid())
		commitEdit(_prevEditRow, _prevEditCol, _editItemContextual->item->property("text"));
}

QQmlContext * DataSetViewBase::setStyleDataItem(QQmlContext * previousContext, bool active, size_t col, size_t row, bool emptyValLabel)
{
	JASPTIMER_SCOPE(DataSetViewBase::setStyleDataItem);

	QModelIndex modelIndex = _model->index(row, col);

	auto	flagsModel	= _model->flags(modelIndex);
	bool	isEditable	( flagsModel & Qt::ItemIsEditable),
			isEnabled	( flagsModel & Qt::ItemIsEnabled);

	if(isEditable || _storedDisplayText.count(row) == 0 || _storedDisplayText[row].count(col) == 0)
		_storedDisplayText[row][col] = _model->data(modelIndex, Qt::DisplayRole).toString();

	QString text		= _storedDisplayText[row][col],
			textEdit	= _model->data(modelIndex, getRole("noSepaDisplay")).toString();

	if(isEditable && !emptyValLabel)
	{
		if(text == tq(EmptyValues::displayString()))
			text = "";

		if(textEdit == tq(EmptyValues::displayString()))
			textEdit = "";
	}
		

	if(previousContext == nullptr)
		previousContext = new QQmlContext(qmlContext(this), this);
	

	//The first four also get updated in DataSetViewBase::modelDataChanged!
	//If adding or changes behaviour also do that there
	previousContext->setContextProperty("itemText",			text);
	previousContext->setContextProperty("itemData",			_model->data(modelIndex));
	previousContext->setContextProperty("itemEnabled",		isEnabled);
	previousContext->setContextProperty("itemTextEdit",		textEdit);
	previousContext->setContextProperty("itemShadowText",	_model->data(modelIndex, getRole("shadowDisplay")));
	previousContext->setContextProperty("itemLabel",		_model->data(modelIndex, getRole("label")));
	previousContext->setContextProperty("itemValue",		_model->data(modelIndex, getRole("value")));
	previousContext->setContextProperty("itemActive",		active);
	previousContext->setContextProperty("itemEditable",		isEditable);
	const QVariant selected = _model->data(modelIndex, getRole("selected"));
	previousContext->setContextProperty("itemSelected",		selected.isValid() && selected.userType() != QMetaType::UnknownType ? selected.toBool() : _selectionModel->isSelected(modelIndex));
	previousContext->setContextProperty("itemFiltered",		_model->data(modelIndex, getRole("filter")));
	previousContext->setContextProperty("itemInputType",	_model->data(modelIndex, getRole("itemInputValue")));
	previousContext->setContextProperty("columnIndex",		static_cast<int>(col));
	previousContext->setContextProperty("rowIndex",			static_cast<int>(row));
	//previousContext->setContextProperty("index",			idx);
	previousContext->setContextProperty("isDynamic",		true);
	previousContext->setContextProperty("tableView",		_tableViewItem);
	previousContext->setContextProperty("dataviewer",		this);

	return previousContext;
}

QQmlContext * DataSetViewBase::setStyleDataRowNumber(QQmlContext * previousContext, int row)
{
	if(previousContext == nullptr)
		previousContext = new QQmlContext(qmlContext(this), this);

	const QVariantMap values = rowHeaderData(row);
	previousContext->setContextProperty("rowIndex",			row);
	previousContext->setContextProperty("rowNumber",		values.value("rowNumber")); //gives original row, could be different from rowIndex if filtered-out-values arent visible
	previousContext->setContextProperty("virtual",			values.value("virtual"));
	previousContext->setContextProperty("headerText",		values.value("headerText"));

	return previousContext;
}

QQmlContext * DataSetViewBase::setStyleDataColumnHeader(QQmlContext * previousContext, int column)
{
	if(previousContext == nullptr)
		previousContext = new QQmlContext(qmlContext(this), this);

	const QVariantMap values = columnHeaderData(column);
	for(auto it = values.constBegin(); it != values.constEnd(); ++it)
		previousContext->setContextProperty(it.key(), it.value());
	previousContext->setContextProperty("columnIndex", column);

	return previousContext;
}

void DataSetViewBase::setItemDelegate(QQmlComponent * newDelegate)
{
	if(newDelegate != _itemDelegate)
	{
		if(_itemDelegate != nullptr)
			delete _itemDelegate;
		_itemDelegate = newDelegate;
		emit itemDelegateChanged();
	}
}

void DataSetViewBase::setRowNumberDelegate(QQmlComponent * newDelegate)
{
	if(newDelegate != _rowNumberDelegate)
	{
		if(_rowNumberDelegate != nullptr)
			delete _rowNumberDelegate;
		_rowNumberDelegate = newDelegate;
		emit rowNumberDelegateChanged();
	}
}


void DataSetViewBase::setColumnHeaderDelegate(QQmlComponent * newDelegate)
{
	if(newDelegate != _columnHeaderDelegate)
	{
		if(_columnHeaderDelegate != nullptr)
			delete _columnHeaderDelegate;
		_columnHeaderDelegate = newDelegate;
		emit columnHeaderDelegateChanged();
	}
}


void DataSetViewBase::setLeftTopCornerItem(QQuickItem * newItem)
{
	if(newItem != _leftTopItem)
	{
		if(_leftTopItem != nullptr)
			delete _leftTopItem;
		_leftTopItem = newItem;

		if(_leftTopItem != nullptr)
		{
			iAmParent(_leftTopItem);

			_leftTopItem->setProperty("text", "?");

			_leftTopItem->setZ(-1);

			_leftTopItem->setVisible(true);

			_leftTopItem->setHeight(_dataRowsMaxHeight);
			_leftTopItem->setWidth(_rowNumberMaxWidth);
			_leftTopItem->setX(_viewportX);
			_leftTopItem->setY(_viewportY);

		}

		emit leftTopCornerItemChanged();
	}
}

void DataSetViewBase::setExtraColumnItem(QQuickItem * newItem)
{
	if (expandDataSet())
		newItem = nullptr;

	if(newItem != _extraColumnItem)
	{
		if(_extraColumnItem != nullptr)
			delete _extraColumnItem;
		_extraColumnItem = newItem;

		if(_extraColumnItem != nullptr)
		{
			iAmParent(_extraColumnItem);

			_extraColumnItem->setZ(-1);

			_extraColumnItem->setVisible(true);

			_extraColumnItem->setHeight(_dataRowsMaxHeight);
			_extraColumnItem->setX(_dataWidth);
			_extraColumnItem->setY(_viewportY);

		}

		emit extraColumnItemChanged();
	}
}

void DataSetViewBase::setCacheItems(bool cacheItems)
{
	if(cacheItems == _cacheItems)
		return;

	_cacheItems = cacheItems;

	emit cacheItemsChanged();
}

void DataSetViewBase::reloadTextItems()
{
	reloadTableView();
}

void DataSetViewBase::reloadRowNumbers()
{
	reloadTableView();
}

void DataSetViewBase::reloadColumnHeaders()
{
	reloadTableView();
}


void DataSetViewBase::myParentChanged(QQuickItem * newParentItem)
{

	/*if(newParentItem->property("viewport").isValid())
	{
		QQuickItem * viewport = newParentItem->property("viewport").
		connect
	}
	void xChanged();
	void yChanged();
	void widthChanged();
	void heightChanged();
*/
}

QPoint DataSetViewBase::editCoordinates() const
{
	return QPoint(_prevEditCol, _prevEditRow);
}

int DataSetViewBase::maxColWidth() const
{
	return _maxColWidth;
}

void DataSetViewBase::setMaxColWidth(int newMaxColWidth)
{
	if (_maxColWidth == newMaxColWidth)
		return;
	_maxColWidth = newMaxColWidth;
	emit maxColWidthChanged();
}
