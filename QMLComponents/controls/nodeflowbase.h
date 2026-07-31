#ifndef NODEFLOWBASE_H
#define NODEFLOWBASE_H

#include "jaspcontrol.h"
#include <QColor>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QHash>
#include <QUndoStack>
#include <QUndoCommand>

class QHoverEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;
class QPainter;
class QSGTransformNode;
class QSGImageNode;
class QSGTexture;
class QQuickWindow;

class NodeFlowBase : public JASPControl
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool gridVisible READ gridVisible WRITE setGridVisible NOTIFY gridVisibleChanged)
    Q_PROPERTY(bool running READ isRunning WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(bool connectionMode READ connectionMode WRITE setConnectionMode NOTIFY connectionModeChanged)
    Q_PROPERTY(double zoom READ zoom NOTIFY zoomChanged)
    Q_PROPERTY(int selectedNodeId READ selectedNodeId NOTIFY nodeSelected)
    Q_PROPERTY(int selectedEdgeIndex READ selectedEdgeIndex NOTIFY edgeIndexSelected)
    Q_PROPERTY(int nodeCount READ nodeCount NOTIFY graphChanged)
    Q_PROPERTY(int edgeCount READ edgeCount NOTIFY graphChanged)

    // Undo/Redo support
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY canRedoChanged)

public:
    struct Node {
        int id = 0;
        QString title;
        QString subtitle;
        QColor color;
        QRectF rect;
    };

    struct Edge {
        int from = 0;
        int to = 0;
        QString label;
    };

    explicit NodeFlowBase(QQuickItem *parent = nullptr);

    // Data
    Q_INVOKABLE void setNodes(const QVariantList &nodes);
    Q_INVOKABLE void setEdges(const QVariantList &edges);
    Q_INVOKABLE QVariantList nodes() const;
    Q_INVOKABLE QVariantList edges() const;

    // Item access
    Q_INVOKABLE QVariantMap node(int id) const;
    Q_INVOKABLE QVariantMap edge(int index) const;
    Q_INVOKABLE QString nodeTitle(int id) const;
    Q_INVOKABLE QString edgeLabel(int index) const;

    // Graph editing
    Q_INVOKABLE int addNode(const QString &title, const QString &subtitle, const QColor &color, const QPointF &position);
    Q_INVOKABLE void addEdge(int from, int to, const QString &label = QString());
    Q_INVOKABLE void removeSelectedItem();
    Q_INVOKABLE void removeSelectedNode();
    Q_INVOKABLE void removeSelectedEdge();
    Q_INVOKABLE void removeEdgesOfSelectedNode();
    Q_INVOKABLE void clearGraph();
    Q_INVOKABLE void fitToView();
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void resetView();
    Q_INVOKABLE bool exportJson(const QString &fileName) const;
    Q_INVOKABLE bool importJson(const QString &fileName);

    // QML callbacks
    Q_INVOKABLE void beginConnectionFrom(int nodeId);
    Q_INVOKABLE void setNodeTitle(int id, const QString &title);
    Q_INVOKABLE void setEdgeLabel(int index, const QString &label);

    // Undo / Redo
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    bool canUndo() const { return m_undoStack.canUndo(); }
    bool canRedo() const { return m_undoStack.canRedo(); }

    // Coordinate conversion
    Q_INVOKABLE QPointF sceneToItem(const QPointF &scenePoint) const;
    Q_INVOKABLE QPointF itemToScene(const QPointF &itemPoint) const;

    bool gridVisible() const { return m_gridVisible; }
    bool isRunning() const { return m_running; }
    bool connectionMode() const { return m_connectionMode; }
    double zoom() const { return m_zoom; }
    void    setGridVisible(bool gridVisible);
    void    setRunning(bool running) { if (m_running != running) { m_running = running; emit runningChanged(m_running); } }
    void    setConnectionMode(bool mode) { if (m_connectionMode != mode) { m_connectionMode = mode; emit connectionModeChanged(mode); } }

    int selectedNodeId() const { return m_selectedNodeId; }
    int selectedEdgeIndex() const { return m_selectedEdgeIndex; }
    int nodeCount() const { return static_cast<int>(m_nodes.size()); }
    int edgeCount() const { return static_cast<int>(m_edges.size()); }

    // Internal operations exposed for Undo Commands
    int addNodeInternal(const QString &title, const QString &subtitle, const QColor &color, const QPointF &position);
    void removeNodeInternal(int id);
    void restoreNodeInternal(const Node &node, const QList<Edge> &edges);
    int addEdgeInternal(int from, int to, const QString &label);
    void setNodePositionInternal(int id, const QPointF &pos);
    void removeEdgeInternal(int index);
    void restoreEdgeInternal(const Edge &edge, int index);

signals:
    void graphChanged(int nodeCount, int edgeCount);
    void nodeSelected(int id, const QString &title);
    void edgeSelected(int from, int to, const QString &label);
    void edgeIndexSelected(int index);
    void zoomChanged(double zoom);
    void gridVisibleChanged(bool visible);
    void runningChanged(bool running);
    void connectionModeChanged(bool enabled);
    void contextMenuRequested(qreal itemX, qreal itemY, int nodeId, int edgeIndex, qreal sceneX, qreal sceneY);
    void nodeTitleEditRequested(int id, const QString &currentTitle);
    void edgeLabelEditRequested(int index, const QString &currentLabel);
    void canUndoChanged();
    void canRedoChanged();


protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

    void setSelectedNode(int id);
    void setSelectedEdge(int index);
    void emitGraphChanged();
    void markGraphDirty();
    void markStateDirty();
    void markGridDirty();

private:
    // Scene Graph builders
    void rebuildGridGraphics();
    void rebuildEdgeGraphics();
    void rebuildNodeGraphics();
    void rebuildDraftEdgeGraphics();

    QRectF nodeScreenRect(const Node &node) const;
    QSGNode* createNodeGraphics(const Node &node, bool selected, bool active);
    QSGNode* createEdgeGraphics(const Edge &edge, bool selected, bool active);
    QSGNode* createDraftEdgeGraphics();
    QSGImageNode* createTextNode(const QString &text, const QFont &font,
                                 const QColor &color, const QRectF &rect,
                                 Qt::Alignment align = Qt::AlignLeft | Qt::AlignVCenter);

    // Helpers
    void cleanupSceneGraphPointers();

    QRectF nodeRect(const Node &node) const;
    QPointF sceneToWidget(const QPointF &point) const;
    QPointF widgetToScene(const QPointF &point) const;
    QRectF sceneToWidget(const QRectF &rect) const;
    int hitNode(const QPointF &scenePoint) const;
    int hitEdge(const QPointF &scenePoint) const;
    Node *findNode(int id);
    const Node *findNode(int id) const;
    QRectF graphBounds() const;
    QPointF nodeAnchor(const Node &node, bool output) const;
    QPainterPath edgePath(const Edge &edge) const;
    void applyZoom(double newZoom);
    void updatePointerScenePosition(const QPointF &itemPos);

    // Serialization
    QVariantMap nodeToVariant(const Node &node) const;
    Node nodeFromVariant(const QVariantMap &map) const;
    QVariantMap edgeToVariant(const Edge &edge) const;
    Edge edgeFromVariant(const QVariantMap &map) const;

    // Scene Graph layer nodes
    QSGTransformNode* m_rootTransform = nullptr;
    QSGNode* m_gridNode = nullptr;
    QSGNode* m_edgeNode = nullptr;
    QSGNode* m_nodeParent = nullptr;
    QSGNode* m_draftEdgeNode = nullptr;

    // Cached window/dpr
    QQuickWindow* m_window = nullptr;
    qreal m_dpr = 1.0;

    // Dirty flags
    bool m_gridDirty = true;        // Grid needs rebuild (pan/zoom/resize)
    bool m_contentDirty = true;     // Nodes/Edges need rebuild

    // Data
    QUndoStack m_undoStack;
    QVector<Node> m_nodes;
    QVector<Edge> m_edges;
    double m_zoom = 1.0;
    QPointF m_panOffset{40.0, 40.0};
    bool m_gridVisible = true;
    bool m_draggingNode = false;
    bool m_panning = false;
    bool m_connecting = false;
    bool m_connectionMode = false;
    bool m_running = false;
    bool m_needInitialFit = true;
    QPointF m_lastWidgetPos;
    QPointF m_lastScenePos;
    QPointF m_dragStartPos;
    QPointF m_connectionEnd;
    int m_selectedNodeId = -1;
    int m_selectedEdgeIndex = -1;
    int m_connectionFromId = -1;
    int m_nextNodeId = 1;
    int m_activeStep = 0;
};

#endif // NODEFLOWBASE_H