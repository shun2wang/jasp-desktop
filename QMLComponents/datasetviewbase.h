#ifndef DATASETVIEWBASE_H
#define DATASETVIEWBASE_H

#include <QObject>
#include <QQuickItem>
#include <QAbstractItemModel>
#include <vector>

#include <map>
#include <QtQml>
#include "qutils.h"

#include <QItemSelectionModel>
#include <QItemSelection>
#include <QIdentityProxyModel>

class DataSetViewBase;

/// Adds the historical JASP delegate roles without changing the data model or
/// the QModelIndexes used by the spreadsheet's selection and undo stack.
class DataSetViewTableModel : public QIdentityProxyModel
{
public:
    enum class Kind { Cells, RowHeaders, ColumnHeaders };
    static constexpr int ItemSelectedRole = Qt::UserRole + 10009;

    DataSetViewTableModel(DataSetViewBase *owner, Kind kind);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    void notifyDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QVector<int> &roles);
    void notifyHeaderDataChanged(Qt::Orientation orientation, int first, int last);

private:
    DataSetViewBase	*_owner;
    Kind			 _kind;
    QHash<int, QByteArray> _extraRoles;
};

//#define DATASETVIEWBASE_DEBUG_VIEWPORT
//#define DATASETVIEWBASE_DEBUG_CREATION

struct ItemContextualized
{
    ItemContextualized(QQmlContext * context = nullptr, QQuickItem * item = nullptr)
    : item(item), context(context)
    {}

    ~ItemContextualized()
    {
        if(item)	item	-> deleteLater();
        if(context)				context	-> deleteLater();

        context =  nullptr;
        item	=  nullptr;
    }

    QQuickItem	* item		= nullptr;
    QQmlContext * context	= nullptr;
};

/// Custom QQuickItem to render data tables witch caching and only displaying the necessary cells and lines
/// Supports scaling the data into millions of columns and rows without any noticable slowdowns (the model could slow it down though)
/// Contains custom rendering code for the lines to make sure they are always a single pixel wide.
/// Caching is a bit flawed at the moment though so when changing data in the model it is best to turn that off.
/// It also uses pools of header-, rowheader- and general-items when they go out of view to avoid the overhead of recreating them all the time.
class DataSetViewBase : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY( QAbstractItemModel	*	model					READ model					WRITE setModel					NOTIFY modelChanged					)
    Q_PROPERTY( QAbstractItemModel	*	tableCellModel			READ tableCellModel			CONSTANT										)
    Q_PROPERTY( QAbstractItemModel	*	tableRowHeaderModel		READ tableRowHeaderModel	CONSTANT										)
    Q_PROPERTY( QAbstractItemModel	*	tableColumnHeaderModel	READ tableColumnHeaderModel	CONSTANT										)
    Q_PROPERTY( int						itemHorizontalPadding	READ itemHorizontalPadding	WRITE setItemHorizontalPadding	NOTIFY itemHorizontalPaddingChanged )
    Q_PROPERTY( int						itemVerticalPadding		READ itemVerticalPadding	WRITE setItemVerticalPadding	NOTIFY itemVerticalPaddingChanged	)
    Q_PROPERTY( double					viewportX				READ viewportX				WRITE setViewportX				NOTIFY viewportXChanged				)
    Q_PROPERTY( double					viewportY				READ viewportY				WRITE setViewportY				NOTIFY viewportYChanged				)
    Q_PROPERTY( double					viewportW				READ viewportW				WRITE setViewportW				NOTIFY viewportWChanged				)
    Q_PROPERTY( double					viewportH				READ viewportH				WRITE setViewportH				NOTIFY viewportHChanged				)
    Q_PROPERTY( QQmlComponent		*	itemDelegate			READ itemDelegate			WRITE setItemDelegate			NOTIFY itemDelegateChanged			)
    Q_PROPERTY( QQmlComponent		*	rowNumberDelegate		READ rowNumberDelegate		WRITE setRowNumberDelegate		NOTIFY rowNumberDelegateChanged		)
    Q_PROPERTY( QQmlComponent		*	columnHeaderDelegate	READ columnHeaderDelegate	WRITE setColumnHeaderDelegate	NOTIFY columnHeaderDelegateChanged	)
    Q_PROPERTY( QQuickItem			*	leftTopCornerItem		READ leftTopCornerItem		WRITE setLeftTopCornerItem		NOTIFY leftTopCornerItemChanged		)
    Q_PROPERTY( QQuickItem			*	extraColumnItem			READ extraColumnItem		WRITE setExtraColumnItem		NOTIFY extraColumnItemChanged		)
    Q_PROPERTY( QQmlComponent		*	editDelegate			READ editDelegate			WRITE setEditDelegate			NOTIFY editDelegateChanged			)
    Q_PROPERTY( double					headerHeight			READ headerHeight											NOTIFY headerHeightChanged			)
    Q_PROPERTY( double					rowNumberWidth			READ rowNumberWidth			WRITE setRowNumberWidth			NOTIFY rowNumberWidthChanged		)
    Q_PROPERTY( bool					cacheItems				READ cacheItems				WRITE setCacheItems				NOTIFY cacheItemsChanged			)
    Q_PROPERTY( QQuickItem			*	tableViewItem			READ tableViewItem			WRITE setTableViewItem												)
    Q_PROPERTY( QItemSelectionModel *	selection				READ selectionModel											NOTIFY selectionModelChanged		)
    Q_PROPERTY(	QPoint					selectionMin			READ selectionMin											NOTIFY selectionMinChanged			)
    Q_PROPERTY(	QPoint					selectionMax			READ selectionMax											NOTIFY selectionMaxChanged			)
    Q_PROPERTY(	QPoint					editCoordinates			READ editCoordinates										NOTIFY editCoordinatesChanged		)
    Q_PROPERTY(	bool					editing					READ editing				WRITE setEditing				NOTIFY editingChanged				)
    Q_PROPERTY( int						maxColWidth				READ maxColWidth			WRITE setMaxColWidth			NOTIFY maxColWidthChanged			)

public:

                            DataSetViewBase(QQuickItem *parent = nullptr);

    virtual void			setModel(QAbstractItemModel * model);

    virtual QAbstractItemModel	*	model()						const	{ return _model;					}
    QAbstractItemModel	*	tableCellModel()					const	{ return _cellModel;				}
    QAbstractItemModel	*	tableRowHeaderModel()				const	{ return _rowHeaderModel;			}
    QAbstractItemModel	*	tableColumnHeaderModel()			const	{ return _columnHeaderModel;		}
    QItemSelectionModel	*	selectionModel()					const	{ return _selectionModel;			}

    int						itemHorizontalPadding()				const	{ return _itemHorizontalPadding;	}
    int						itemVerticalPadding()				const	{ return _itemVerticalPadding;		}
    double					headerHeight()						const	{ return _dataRowsMaxHeight;		}
    double					rowNumberWidth()					const	{ return _rowNumberMaxWidth;		}

    double					viewportX()							const	{ return _viewportX;				}
    double					viewportY()							const	{ return _viewportY;				}
    double					viewportW()							const	{ return _viewportW;				}
    double					viewportH()							const	{ return _viewportH;				}

    QQmlComponent		*	itemDelegate()						const	{ return _itemDelegate;				}
    QQmlComponent		*	rowNumberDelegate()					const	{ return _rowNumberDelegate;		}
    QQmlComponent		*	columnHeaderDelegate()				const	{ return _columnHeaderDelegate;		}
    QQuickItem			*	leftTopCornerItem()					const	{ return _leftTopItem;				}
    QQuickItem			*	extraColumnItem()					const	{ return _extraColumnItem;			}
    QQuickItem			*	tableViewItem()						const	{ return _tableViewItem;			}
    QQmlComponent		*	editDelegate()						const	{ return _editDelegate;				}

    bool					cacheItems()						const	{ return _cacheItems;				}
    QPoint					selectionMin()						const;
    QPoint					selectionMax()						const;
    bool					editing()							const	{ return _editing;					}

    Q_INVOKABLE QQuickItem*	getColumnHeader(int col)					{ return _columnHeaderItems.value(col, nullptr);	}
    Q_INVOKABLE QQuickItem*	getRowHeader(	int row)					{ return _rowNumberItems.value(row, nullptr);		}
    Q_INVOKABLE QQuickItem*	createDelegate(const QString &kind, QQmlComponent *component, QQuickItem *parent, int row, int column);
    Q_INVOKABLE void		destroyDelegate(const QString &kind, QQuickItem *item);

    Q_INVOKABLE	double		columnWidth(int column)				const	{ return column >= 0 && column < int(_dataColsMaxWidth.size()) ? _dataColsMaxWidth[column] : 0; }
    Q_INVOKABLE	double		columnX(int column)					const	{ return column >= 0 && column < int(_colXPositions.size()) ? _colXPositions[column] : 0; }
    Q_INVOKABLE	int			lineFlags(int row, int column)		const	{ return _model ? _model->data(_model->index(row, column), getRole("lines")).toInt() : 0; }
    QQuickItem *	renderer() const						{ return _tableView; }
    QQuickItem *	cellItem(int row, int column) const;
    QVariantMap	cellData(int row, int column) const;
    QVariantMap	rowHeaderData(int row) const;
    QVariantMap	columnHeaderData(int column) const;

    Q_INVOKABLE	bool		clipBoardPasteIsCells()				const;

    GENERIC_SET_FUNCTION(ViewportX,		_viewportX,		viewportXChanged,	double	)
    GENERIC_SET_FUNCTION(ViewportY,		_viewportY,		viewportYChanged,	double	)
    GENERIC_SET_FUNCTION(ViewportW,		_viewportW,		viewportWChanged,	double	)
    GENERIC_SET_FUNCTION(ViewportH,		_viewportH,		viewportHChanged,	double	)

    void setItemHorizontalPadding(	int newHorizontalPadding)	{ if(newHorizontalPadding	!= _itemHorizontalPadding)	{ _itemHorizontalPadding	= newHorizontalPadding;		emit itemHorizontalPaddingChanged();	update(); } }
    void setItemVerticalPadding(	int newVerticalPadding)		{ if(newVerticalPadding		!= _itemVerticalPadding)	{ _itemVerticalPadding		= newVerticalPadding;		emit itemVerticalPaddingChanged();		update(); } }

    void setRowNumberDelegate(		QQmlComponent	* newDelegate);
    void setColumnHeaderDelegate(	QQmlComponent	* newDelegate);
    QQmlContext * setStyleDataRowNumber(QQmlContext * previousContext, int row);
    QQmlContext * setStyleDataColumnHeader(QQmlContext * previousContext, int column);
    void			updateDelegateData();
    void			setDelegateGeometry(const QString &kind, QQuickItem *item, int row, int column);
    void setItemDelegate(			QQmlComponent	* newDelegate);
    void setLeftTopCornerItem(		QQuickItem		* newItem);
    void setExtraColumnItem(		QQuickItem		* newItem);
    void setEditDelegate(			QQmlComponent	* editDelegate);
    void		setTableViewItem(			QQuickItem		* tableViewItem) { _tableViewItem = tableViewItem; }
    void		setCacheItems(				bool			  cacheItems);
    void		initializeTableView();
    bool		event(QEvent *event) override;
    void		componentComplete() override;
    void		reloadTableView();

    void resetItems();

    GENERIC_SET_FUNCTION(HeaderHeight,		_dataRowsMaxHeight, headerHeightChanged,		double)
    GENERIC_SET_FUNCTION(RowNumberWidth,	_rowNumberMaxWidth, rowNumberWidthChanged,		double)


    QPoint editCoordinates() const;

    int maxColWidth() const;
    void setMaxColWidth(int newMaxColWidth);

    virtual	bool expandDataSet() const { return false; }

signals:
    void		modelChanged();
    void		selectionModelChanged();
    void		itemHorizontalPaddingChanged();
    void		itemVerticalPaddingChanged();

    void		viewportXChanged();
    void		viewportYChanged();
    void		viewportWChanged();
    void		viewportHChanged();

    void		rowNumberDelegateChanged();
    void		columnHeaderDelegateChanged();
    void		itemDelegateChanged();
    void		leftTopCornerItemChanged();
    void		extraColumnItemChanged();
    void		editDelegateChanged(QQmlComponent * editDelegate);

    void		itemSizeChanged();
    void		selectionChanged();

    void		headerHeightChanged();
    void		rowNumberWidthChanged();

    void		cacheItemsChanged();

    void		selectionMinChanged();
    void		selectionMaxChanged();

    void		editingChanged(bool shiftSelectActive);

    void		selectionBudgesUp();
    void		selectionBudgesDown();
    void		selectionBudgesLeft();
    void		selectionBudgesRight();
    void		editCoordinatesChanged();

    void		maxColWidthChanged();

public slots:
    void		calculateCellSizes()	{ calculateCellSizesAndClear(false); }
    void		aContentSizeChanged()	{ _recalculateCellSizes = true; }
    void		viewportChanged();
    void		viewportChangedDelayed();
    void		myParentChanged(QQuickItem *);

    void		reloadTextItems();
    void		reloadRowNumbers();
    void		reloadColumnHeaders();

    void		modelDataChanged(const QModelIndex &, const QModelIndex &, const QVector<int> &);
    void		modelHeaderDataChanged(Qt::Orientation, int, int);
    void		modelAboutToBeReset();
    void		modelWasReset();
    void		setExtraColumnX();

    bool		isSelected(			int row, int col);
    void		pollSelectScroll(	int row, int column);
    void		setEditing(bool shiftSelectActive);
    bool		relaxForSelectScroll();

    virtual bool isVirtual			(const QPoint& point)	{ return false; }
    virtual bool isRowVirtual		(int row)				{ return false;	}
    virtual bool isColumnVirtual	(int column)			{ return false;	}
    virtual bool isFiltered			(int row, int col)		{ return true;	}
    bool		isColumnHeader		(const QPoint& p)		{ return p.x() >= 0	&& p.y() == -1; }
    bool		isRowHeader			(const QPoint& p)		{ return p.x() == -1	&& p.y() >= 0;	}
    bool		isCell				(const QPoint& p)		{ return p.x() >= 0	&& p.y() >= 0;	}

    void		select(						int row, int column,	bool shiftPressed,			bool ctrlCmdPressed);
    void		selectionClear();
    void		selectHover(				int row, int column);

    void		columnsAboutToBeInserted(	const QModelIndex &parent, int first, int last);
    void		columnsAboutToBeRemoved(	const QModelIndex &parent, int first, int last);
    void		rowsAboutToBeInserted(		const QModelIndex &parent, int first, int last);
    void		rowsAboutToBeRemoved(		const QModelIndex &parent, int first, int last);
    void		columnsInserted(			const QModelIndex &parent, int first, int last);
    void		columnsRemoved(				const QModelIndex &parent, int first, int last);
    void		rowsInserted(				const QModelIndex &parent, int first, int last);
    void		rowsRemoved(				const QModelIndex &parent, int first, int last);

    int			rowCount()								{ return _model->rowCount();	}
    int			columnCount()							{ return _model->columnCount(); }

    void		selectAll();
    void		clearEdit();
    void		edit(int row, int column);
    void		destroyEditItem(bool restoreItem=true);
    void		commitEdit(				int row, int column, QVariant editedValue);
    void		onDataModeChanged(bool dataMode);
    void		commitLastEdit();

protected:
    void		calculateCellSizesAndClear(bool clearStorage);
    void		determineCurrentViewPortIndices();
    void		iAmParent(QQuickItem *item);
    void		buildNewLinesAndCreateNewItems();
    void		columnIndexSelectedApply(int columnIndex, std::function<void (int)> applyThis);
    void		columnIndexSelectedApply(int columnIndex, std::function<void (intset)> applyThis);

    float extraColumnWidth() { return !_extraColumnItem || expandDataSet() ? 0 : _extraColumnItem->width(); }

    QQuickItem	*	createleftTopCorner();

    void			updateExtraColumnItem();
    void			positionEditItem(	int row, int col);
    void			positionItem(		int row, int col, QQuickItem *item);

    QQmlContext *	setStyleDataItem(			QQmlContext * previousContext, bool active, size_t col, size_t row, bool emptyValLabel = true);
    void			selectionDataChanged(		const QItemSelection &selected, const QItemSelection &deselected);
    void			notifyItemSelected(			const QModelIndex &topLeft, const QModelIndex &bottomRight);

    QSizeF			getTextSize(const QString& text)	const;
    QSizeF			getColumnSize(int col);
    QSizeF			getRowHeaderSize();
    void			clearCaches();
    virtual void	setRolenames();
    int				getRole(const std::string& roleName)														const;



protected:
    QAbstractItemModel									*	_model					= nullptr;
    DataSetViewTableModel								*	_cellModel				= nullptr,
                                                        *	_rowHeaderModel			= nullptr,
                                                        *	_columnHeaderModel		= nullptr;
    QItemSelectionModel									*	_selectionModel			= nullptr;
    std::vector<QSizeF>										_cellSizes;							//[col]
    std::vector<double>										_colXPositions,						//[col][row]
                                                            _dataColsMaxWidth;
    QHash<int, QQuickItem *>									_rowNumberItems, _columnHeaderItems;
    QHash<QQuickItem *, QPoint>								_cellItems;
    QHash<QQuickItem *, QQmlContext *>						_delegateContexts;
    QQuickItem											*	_leftTopItem			= nullptr,
                                                        *	_extraColumnItem		= nullptr,
                                                        *	_tableViewItem			= nullptr,
                                                        *	_tableView				= nullptr;
    QQmlComponent										*	_itemDelegate			= nullptr,
                                                        *	_rowNumberDelegate		= nullptr,
                                                        *	_columnHeaderDelegate	= nullptr,
                                                        *	_leftTopCornerDelegate	= nullptr,
                                                        *	_styleDataCreator		= nullptr,
                                                        *	_editDelegate			= nullptr;
    ItemContextualized									*	_editItemContextual		= nullptr;
    std::map<size_t, std::map<size_t, QString>>				_storedDisplayText;
    bool													_cacheItems				= false,
                                                            _recalculateCellSizes	= false,
                                                            _ignoreViewpoint		= true,
                                                            _editing				= false;
    double													_dataRowsMaxHeight,
                                                            _dataWidth				= -1,
                                                            _rowNumberMaxWidth		= 0,
                                                            _viewportX				= 0,
                                                            _viewportY				= 0,
                                                            _viewportW				= 1,
                                                            _viewportH				= 1;
    int														_itemHorizontalPadding	= 8,
                                                            _itemVerticalPadding	= 8,
                                                            _previousViewportColMin = -1,
                                                            _previousViewportColMax = -1,
                                                            _previousViewportRowMin = -1,
                                                            _previousViewportRowMax = -1,
                                                            _viewportMargin			=  1,
                                                            _currentViewportColMin	= -1,
                                                            _currentViewportColMax	= -1,
                                                            _currentViewportRowMin	= -1,
                                                            _currentViewportRowMax	= -1,
                                                            _prevEditRow			= -1,
                                                            _prevEditCol			= -1,
                                                            _maxColWidth			= -1;
    int64_t													_selectScrollMs			= 0;
    std::vector<Json::Value>								_copiedColumns;
    QString													_lastJaspCopyIntoClipboard;
    std::vector<qstringvec>									_lastJaspCopyValues,
                                                            _lastJaspCopyLabels;
    std::vector<boolvec>									_lastJaspCopySelect;
    QTimer												*	_delayViewportChangedTimer	= nullptr,
                                                        *	_resetLessTimer				= nullptr;
    strintmap												_roleNameToRole;


};



#endif // DATASETVIEWBASE_H
