#include "nodeflowbase.h"

#include <QFile>
#include <QHoverEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QSizeF>
#include <QTimer>
#include <QWheelEvent>
#include <QSGSimpleTextureNode>
#include <QSGImageNode>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <qcursor.h>

namespace
{
constexpr double MinZoom = 0.25;
constexpr double MaxZoom = 3.5;
constexpr double NodeWidth = 168.0;
constexpr double NodeHeight = 78.0;

QColor statusColor(int index)
{
    switch (index % 5) {
    case 1: return QColor(0xFF1F9D63);
    case 2: return QColor(0xFFD87516);
    case 3: return QColor(0xFF7A5CFA);
    case 4: return QColor(0xFFE9573F);
    default: return QColor(0xFF2E7DD1);
    }
}

// ============================================================================
// Undo Commands
// ============================================================================
class AddNodeCommand : public QUndoCommand {
public:
    AddNodeCommand(NodeFlowBase* flow, const QString& title, const QString& subtitle, const QColor& color, const QPointF& pos)
        : m_flow(flow), m_title(title), m_subtitle(subtitle), m_color(color), m_pos(pos) {
        setText(QObject::tr("Add Node"));
    }
    void undo() override { m_flow->removeNodeInternal(m_id); }
    void redo() override { m_id = m_flow->addNodeInternal(m_title, m_subtitle, m_color, m_pos); }
    int nodeId() const { return m_id; }
private:
    NodeFlowBase* m_flow;
    QString m_title, m_subtitle;
    QColor m_color;
    QPointF m_pos;
    int m_id = -1;
};

class RemoveNodeCommand : public QUndoCommand {
public:
    RemoveNodeCommand(NodeFlowBase* flow, const NodeFlowBase::Node& node, const QList<NodeFlowBase::Edge>& edges)
        : m_flow(flow), m_node(node), m_edges(edges) {
        setText(QObject::tr("Remove Node"));
    }
    void undo() override { m_flow->restoreNodeInternal(m_node, m_edges); }
    void redo() override { m_flow->removeNodeInternal(m_node.id); }
private:
    NodeFlowBase* m_flow;
    NodeFlowBase::Node m_node;
    QList<NodeFlowBase::Edge> m_edges;
};

class AddEdgeCommand : public QUndoCommand {
public:
    AddEdgeCommand(NodeFlowBase* flow, int from, int to, const QString& label)
        : m_flow(flow), m_from(from), m_to(to), m_label(label) {
        setText(QObject::tr("Add Edge"));
    }
    void undo() override { m_flow->removeEdgeInternal(m_index); }
    void redo() override { m_index = m_flow->addEdgeInternal(m_from, m_to, m_label); }
private:
    NodeFlowBase* m_flow;
    int m_from, m_to;
    QString m_label;
    int m_index = -1;
};

class RemoveEdgeCommand : public QUndoCommand {
public:
    RemoveEdgeCommand(NodeFlowBase* flow, const NodeFlowBase::Edge& edge, int index)
        : m_flow(flow), m_edge(edge), m_index(index) {
        setText(QObject::tr("Remove Edge"));
    }
    void undo() override { m_flow->restoreEdgeInternal(m_edge, m_index); }
    void redo() override { m_flow->removeEdgeInternal(m_index); }
private:
    NodeFlowBase* m_flow;
    NodeFlowBase::Edge m_edge;
    int m_index;
};

class MoveNodeCommand : public QUndoCommand {
public:
    MoveNodeCommand(NodeFlowBase* flow, int id, const QPointF& oldPos, const QPointF& newPos)
        : m_flow(flow), m_id(id), m_oldPos(oldPos), m_newPos(newPos) {
        setText(QObject::tr("Move Node"));
    }
    void undo() override { m_flow->setNodePositionInternal(m_id, m_oldPos); }
    void redo() override { m_flow->setNodePositionInternal(m_id, m_newPos); }
private:
    NodeFlowBase* m_flow;
    int m_id;
    QPointF m_oldPos, m_newPos;
};
}

NodeFlowBase::NodeFlowBase(QQuickItem *parent)
    : JASPControl(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setAcceptHoverEvents(true);
    setImplicitWidth(660);
    setImplicitHeight(460);

    auto *timer = new QTimer(this);
    timer->setInterval(850);
    connect(timer, &QTimer::timeout, this, [this]() {
        if (!m_running || m_nodes.isEmpty()) return;
        const int count = std::max(1, static_cast<int>(m_nodes.size()));
        m_activeStep = (m_activeStep + 1) % count;
        markStateDirty();
    });
    timer->start();

    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *win) {
        if (win) {
            m_dpr = win->effectiveDevicePixelRatio();
            markGraphDirty();
        }
    });

    connect(&m_undoStack, &QUndoStack::canUndoChanged, this, &NodeFlowBase::canUndoChanged);
    connect(&m_undoStack, &QUndoStack::canRedoChanged, this, &NodeFlowBase::canRedoChanged);
}

void NodeFlowBase::markGraphDirty() {
    m_gridDirty = true;
    m_contentDirty = true;
    update();
}

void NodeFlowBase::markStateDirty() {
    m_contentDirty = true;
    update();
}

void NodeFlowBase::markGridDirty() {
    m_gridDirty = true;
    update();
}

QSGNode *NodeFlowBase::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    if (width() <= 0 || height() <= 0) {
        delete oldNode;
        cleanupSceneGraphPointers();
        return nullptr;
    }

    if (window() && m_window != window()) {
        m_window = window();
        m_dpr = m_window->effectiveDevicePixelRatio();
        m_gridDirty = true;
        m_contentDirty = true;
    }

    QSGTransformNode *root = static_cast<QSGTransformNode*>(oldNode);
    if (!root) {
        root = new QSGTransformNode();
        m_rootTransform = root;

        m_gridNode = new QSGNode();
        root->appendChildNode(m_gridNode);

        m_edgeNode = new QSGNode();
        root->appendChildNode(m_edgeNode);

        m_nodeParent = new QSGNode();
        root->appendChildNode(m_nodeParent);

        m_draftEdgeNode = new QSGNode();
        root->appendChildNode(m_draftEdgeNode);

        m_gridDirty = true;
        m_contentDirty = true;
    }

    // Update transform (Always cheap to update)
    QMatrix4x4 matrix;
    matrix.translate(m_panOffset.x(), m_panOffset.y());
    matrix.scale(m_zoom);
    root->setMatrix(matrix);

    if (m_gridDirty) {
        rebuildGridGraphics();
        m_gridDirty = false;
    }

    if (m_contentDirty) {
        rebuildEdgeGraphics();
        rebuildNodeGraphics();
        rebuildDraftEdgeGraphics();
        m_contentDirty = false;
    }

    return root;
}

void NodeFlowBase::cleanupSceneGraphPointers()
{
    m_rootTransform = nullptr;
    m_gridNode = nullptr;
    m_edgeNode = nullptr;
    m_nodeParent = nullptr;
    m_draftEdgeNode = nullptr;
}

void NodeFlowBase::rebuildGridGraphics()
{
    if (!m_gridNode || !window()) return;

    while (auto *child = m_gridNode->firstChild()) {
        m_gridNode->removeChildNode(child);
        delete child;
    }

    if (!m_gridVisible) return;

    // Calculate scene bounds visible in the viewport
    QPointF topLeft = widgetToScene(QPointF(0, 0));
    QPointF bottomRight = widgetToScene(QPointF(width(), height()));
    QRectF sceneBounds(topLeft, bottomRight);
    if (sceneBounds.width() <= 0 || sceneBounds.height() <= 0) return;

    double step = 32.0; // Logical step
    double startX = std::floor(sceneBounds.left() / step) * step;
    double startY = std::floor(sceneBounds.top() / step) * step;

    qreal scaleFactor = m_dpr * m_zoom;
    int imgW = static_cast<int>(std::ceil(sceneBounds.width() * scaleFactor));
    int imgH = static_cast<int>(std::ceil(sceneBounds.height() * scaleFactor));
    if (imgW <= 0 || imgH <= 0) return;

    QImage img(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(scaleFactor);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setPen(QPen(QColor("#E5EBF0"), 1));
    for (double x = startX; x <= sceneBounds.right(); x += step) {
        double localX = x - sceneBounds.left();
        p.drawLine(QPointF(localX, 0), QPointF(localX, sceneBounds.height()));
    }
    for (double y = startY; y <= sceneBounds.bottom(); y += step) {
        double localY = y - sceneBounds.top();
        p.drawLine(QPointF(0, localY), QPointF(sceneBounds.width(), localY));
    }
    p.end();

    auto *tex = window()->createTextureFromImage(img, QQuickWindow::TextureHasAlphaChannel);
    if (!tex) return;

    auto *node = window()->createImageNode();
    node->setTexture(tex);
    node->setRect(sceneBounds);
    node->setFiltering(QSGTexture::Linear);
    m_gridNode->appendChildNode(node);
}

void NodeFlowBase::rebuildNodeGraphics()
{
    if (!m_nodeParent) return;

    while (auto *child = m_nodeParent->firstChild()) {
        m_nodeParent->removeChildNode(child);
        delete child;
    }

    for (int i = 0; i < m_nodes.size(); ++i) {
        const Node &node = m_nodes[i];
        bool selected = (node.id == m_selectedNodeId);
        bool active = m_running && (i == m_activeStep % std::max(1, nodeCount()));
        QSGNode *graphic = createNodeGraphics(node, selected, active);
        if (graphic) m_nodeParent->appendChildNode(graphic);
    }
}

void NodeFlowBase::setGridVisible(bool gridVisible)
{
    if (gridVisible == m_gridVisible) return;
    m_gridVisible = gridVisible;
    markGridDirty();
    emit gridVisibleChanged(gridVisible);
}

QSGNode* NodeFlowBase::createNodeGraphics(const Node &node, bool selected, bool active)
{
    if (!window()) return nullptr;

    auto *group = new QSGNode();
    QRectF rect = nodeRect(node);

    // Render at physical resolution to avoid blurring on zoom
    qreal scaleFactor = m_dpr * m_zoom;
    int bgW = static_cast<int>(std::ceil(NodeWidth * scaleFactor));
    int bgH = static_cast<int>(std::ceil(NodeHeight * scaleFactor));

    QImage bgImg(bgW, bgH, QImage::Format_ARGB32_Premultiplied);
    bgImg.setDevicePixelRatio(scaleFactor);
    bgImg.fill(Qt::transparent);

    {
        QPainter p(&bgImg);
        p.setRenderHint(QPainter::Antialiasing);
        QColor border = selected ? QColor("#111827") : node.color.darker(115);
        if (active) border = QColor("#E9573F");
        p.setPen(QPen(border, selected || active ? 3 : 2));
        p.setBrush(Qt::white);
        p.drawRoundedRect(QRectF(0, 0, NodeWidth, NodeHeight), 8, 8);
        p.setPen(Qt::NoPen);
        p.setBrush(node.color);
        p.drawRoundedRect(QRectF(0, 0, 9, NodeHeight), 4, 4);
    }

    auto *bgNode = window()->createImageNode();
    bgNode->setTexture(window()->createTextureFromImage(bgImg, QQuickWindow::TextureHasAlphaChannel));
    bgNode->setRect(rect);
    bgNode->setFiltering(QSGTexture::Linear);
    group->appendChildNode(bgNode);

    QFont titleFont;
    titleFont.setPointSizeF(11.0);
    titleFont.setBold(true);
    auto *titleNode = createTextNode(node.title, titleFont, QColor("#1F2933"),
                                     QRectF(rect.topLeft() + QPointF(20, 13), QSizeF(NodeWidth - 54, 24)));
    if (titleNode) group->appendChildNode(titleNode);

    if (!node.subtitle.isEmpty()) {
        QFont subFont;
        subFont.setPointSizeF(9.0);
        auto *subNode = createTextNode(node.subtitle, subFont, QColor("#66727C"),
                                       QRectF(rect.topLeft() + QPointF(20, 38), QSizeF(NodeWidth - 32, 30)));
        if (subNode) group->appendChildNode(subNode);
    }

    int dotW = static_cast<int>(std::ceil(12 * scaleFactor));
    QImage dotImg(dotW, dotW, QImage::Format_ARGB32_Premultiplied);
    dotImg.setDevicePixelRatio(scaleFactor);
    dotImg.fill(Qt::transparent);
    {
        QPainter p(&dotImg);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(active ? QColor("#E9573F") : node.color.lighter(120));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(0, 0, 12, 12));
    }
    auto *dotNode = window()->createImageNode();
    dotNode->setTexture(window()->createTextureFromImage(dotImg, QQuickWindow::TextureHasAlphaChannel));
    dotNode->setRect(QRectF(rect.right() - 26, rect.top() + 14, 12, 12));
    group->appendChildNode(dotNode);

    auto addPort = [&](const QPointF &scenePos) {
        int pw = static_cast<int>(std::ceil(8 * scaleFactor));
        QImage portImg(pw, pw, QImage::Format_ARGB32_Premultiplied);
        portImg.setDevicePixelRatio(scaleFactor);
        portImg.fill(Qt::transparent);
        {
            QPainter p(&portImg);
            p.setRenderHint(QPainter::Antialiasing);
            p.setBrush(QColor(0xFFCDD7DF));
            p.setPen(QPen(Qt::white, 1));
            p.drawEllipse(QRectF(0, 0, 8, 8));
        }
        auto *portNode = window()->createImageNode();
        portNode->setTexture(window()->createTextureFromImage(portImg, QQuickWindow::TextureHasAlphaChannel));
        portNode->setRect(QRectF(scenePos.x() - 4, scenePos.y() - 4, 8, 8));
        group->appendChildNode(portNode);
    };
    addPort(nodeAnchor(node, false));
    addPort(nodeAnchor(node, true));

    return group;
}

QSGImageNode* NodeFlowBase::createTextNode(const QString &text, const QFont &font,
                                           const QColor &color, const QRectF &rect,
                                           Qt::Alignment align)
{
    if (text.isEmpty() || !window() || rect.width() <= 0 || rect.height() <= 0)
        return nullptr;

    qreal scaleFactor = m_dpr * m_zoom;
    int texW = static_cast<int>(std::ceil(rect.width() * scaleFactor));
    int texH = static_cast<int>(std::ceil(rect.height() * scaleFactor));
    if (texW <= 0 || texH <= 0) return nullptr;

    QImage img(texW, texH, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(scaleFactor);
    img.fill(Qt::transparent);

    {
        QPainter p(&img);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setFont(font);
        p.setPen(color);
        p.drawText(QRectF(0, 0, rect.width(), rect.height()), align, text);
    }

    auto *node = window()->createImageNode();
    QSGTexture *tex = window()->createTextureFromImage(img, QQuickWindow::TextureHasAlphaChannel);
    if (!tex) {
        delete node;
        return nullptr;
    }
    node->setTexture(tex);
    node->setFiltering(QSGTexture::Linear);
    node->setRect(rect);
    return node;
}

QSGNode* NodeFlowBase::createEdgeGraphics(const Edge &edge, bool selected, bool active)
{
    const Node *from = findNode(edge.from);
    const Node *to = findNode(edge.to);
    if (!from || !to || !window()) return new QSGNode();

    auto *group = new QSGNode();
    QPainterPath path = edgePath(edge);
    QRectF bounds = path.boundingRect().adjusted(-8, -8, 8, 8);
    if (bounds.width() <= 0 || bounds.height() <= 0) return group;

    qreal scaleFactor = m_dpr * m_zoom;
    int imgW = static_cast<int>(std::ceil(bounds.width() * scaleFactor));
    int imgH = static_cast<int>(std::ceil(bounds.height() * scaleFactor));

    if (imgW > 8192 || imgH > 8192) return group; // Prevent extreme memory usage

    QImage img(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(scaleFactor);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(-bounds.topLeft());

    if (selected) {
        p.setPen(QPen(QColor("#111827"), 6));
        p.drawPath(path);
    }

    QPen pen(active ? QColor("#E9573F") : QColor("#8EA0AD"), active || selected ? 4 : 2);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    QPointF end = nodeAnchor(*to, false);
    QLineF tail1(QPointF(end.x() - 16, end.y() - 7), end);
    QLineF tail2(QPointF(end.x() - 16, end.y() + 7), end);
    p.drawLine(tail1);
    p.drawLine(tail2);
    p.end();

    auto *imgNode = window()->createImageNode();
    imgNode->setTexture(window()->createTextureFromImage(img, QQuickWindow::TextureHasAlphaChannel));
    imgNode->setRect(bounds);
    imgNode->setFiltering(QSGTexture::Linear);
    group->appendChildNode(imgNode);

    if (!edge.label.isEmpty()) {
        QPointF center = path.pointAtPercent(0.5);
        QRectF labelRect(center.x() - 36, center.y() - 12, 72, 24);

        int bgW = static_cast<int>(std::ceil(72 * scaleFactor));
        int bgH = static_cast<int>(std::ceil(24 * scaleFactor));
        QImage bgImg(bgW, bgH, QImage::Format_ARGB32_Premultiplied);
        bgImg.setDevicePixelRatio(scaleFactor);
        bgImg.fill(Qt::transparent);
        {
            QPainter bp(&bgImg);
            bp.setRenderHint(QPainter::Antialiasing);
            bp.setPen(Qt::NoPen);
            bp.setBrush(active ? QColor("#FFE8E3") : QColor("#FFFFFF"));
            bp.drawRoundedRect(QRectF(0, 0, 72, 24), 4, 4);
        }
        auto *bgNode = window()->createImageNode();
        bgNode->setTexture(window()->createTextureFromImage(bgImg, QQuickWindow::TextureHasAlphaChannel));
        bgNode->setRect(labelRect);
        group->appendChildNode(bgNode);

        QFont labelFont;
        labelFont.setPointSizeF(9.0);
        auto *textNode = createTextNode(edge.label, labelFont,
                                        active ? QColor("#C7442F") : QColor("#66727C"), labelRect, Qt::AlignCenter);
        if (textNode) group->appendChildNode(textNode);
    }

    return group;
}

QSGNode* NodeFlowBase::createDraftEdgeGraphics()
{
    if (m_connectionFromId < 0 || !window()) return new QSGNode();

    const Node *from = findNode(m_connectionFromId);
    if (!from) return new QSGNode();

    QPointF start = nodeAnchor(*from, true);
    QPointF end = m_connectionEnd;
    QPainterPath path;
    path.moveTo(start);
    double handle = std::max(80.0, std::abs(end.x() - start.x()) * 0.45);
    path.cubicTo(QPointF(start.x() + handle, start.y()),
                 QPointF(end.x() - handle, end.y()),
                 end);

    QRectF bounds = path.boundingRect().adjusted(-4, -4, 4, 4);
    if (bounds.width() <= 0 || bounds.height() <= 0) return new QSGNode();

    qreal scaleFactor = m_dpr * m_zoom;
    int imgW = static_cast<int>(std::ceil(bounds.width() * scaleFactor));
    int imgH = static_cast<int>(std::ceil(bounds.height() * scaleFactor));

    QImage img(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(scaleFactor);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(-bounds.topLeft());
    p.setPen(QPen(QColor("#2E7DD1"), 2, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.end();

    auto *node = window()->createImageNode();
    node->setTexture(window()->createTextureFromImage(img, QQuickWindow::TextureHasAlphaChannel));
    node->setRect(bounds);
    return node;
}

void NodeFlowBase::rebuildDraftEdgeGraphics()
{
    if (!m_draftEdgeNode) return;
    while (auto *child = m_draftEdgeNode->firstChild()) {
        m_draftEdgeNode->removeChildNode(child);
        delete child;
    }
    if (m_connecting) {
        auto *draft = createDraftEdgeGraphics();
        if (draft) m_draftEdgeNode->appendChildNode(draft);
    }
}

void NodeFlowBase::rebuildEdgeGraphics()
{
    if (!m_edgeNode) return;
    while (auto *child = m_edgeNode->firstChild()) {
        m_edgeNode->removeChildNode(child);
        delete child;
    }
    for (int i = 0; i < m_edges.size(); ++i) {
        bool selected = (i == m_selectedEdgeIndex);
        bool active = m_running && (i == m_activeStep % std::max(1, edgeCount()));
        QSGNode *edgeGraphic = createEdgeGraphics(m_edges[i], selected, active);
        if (edgeGraphic) m_edgeNode->appendChildNode(edgeGraphic);
    }
}

// ============================================================================
// Serialization
// ============================================================================

QVariantMap NodeFlowBase::nodeToVariant(const Node &node) const
{
    QVariantMap map;
    map.insert(QStringLiteral("id"), node.id);
    map.insert(QStringLiteral("title"), node.title);
    map.insert(QStringLiteral("subtitle"), node.subtitle);
    map.insert(QStringLiteral("color"), node.color.name(QColor::HexRgb));
    map.insert(QStringLiteral("x"), node.rect.x());
    map.insert(QStringLiteral("y"), node.rect.y());
    map.insert(QStringLiteral("width"), node.rect.width() > 0 ? node.rect.width() : NodeWidth);
    map.insert(QStringLiteral("height"), node.rect.height() > 0 ? node.rect.height() : NodeHeight);
    return map;
}

NodeFlowBase::Node NodeFlowBase::nodeFromVariant(const QVariantMap &map) const
{
    Node node;
    node.id = map.value(QStringLiteral("id"), 0).toInt();
    node.title = map.value(QStringLiteral("title")).toString();
    node.subtitle = map.value(QStringLiteral("subtitle")).toString();
    const QColor color(map.value(QStringLiteral("color")).toString());
    node.color = color.isValid() ? color : statusColor(node.id);
    const double x = map.value(QStringLiteral("x"), 0.0).toDouble();
    const double y = map.value(QStringLiteral("y"), 0.0).toDouble();
    const double w = map.value(QStringLiteral("width"), NodeWidth).toDouble();
    const double h = map.value(QStringLiteral("height"), NodeHeight).toDouble();
    node.rect = QRectF(x, y, w, h);
    return node;
}

QVariantMap NodeFlowBase::edgeToVariant(const Edge &edge) const
{
    QVariantMap map;
    map.insert(QStringLiteral("from"), edge.from);
    map.insert(QStringLiteral("to"), edge.to);
    map.insert(QStringLiteral("label"), edge.label);
    return map;
}

NodeFlowBase::Edge NodeFlowBase::edgeFromVariant(const QVariantMap &map) const
{
    Edge edge;
    edge.from = map.value(QStringLiteral("from"), 0).toInt();
    edge.to = map.value(QStringLiteral("to"), 0).toInt();
    edge.label = map.value(QStringLiteral("label")).toString();
    return edge;
}

// ============================================================================
// Data Management & Undo Stack Integration
// ============================================================================

void NodeFlowBase::setNodes(const QVariantList &nodes)
{
    m_nodes.clear();
    m_nodes.reserve(nodes.size());
    for (const QVariant &v : nodes) {
        m_nodes.push_back(nodeFromVariant(v.toMap()));
    }
    m_selectedNodeId = -1;
    m_selectedEdgeIndex = -1;
    m_nextNodeId = 1;
    for (const Node &node : m_nodes) {
        m_nextNodeId = std::max(m_nextNodeId, node.id + 1);
    }
    m_needInitialFit = true;
    m_undoStack.clear();
    emitGraphChanged();
    markGraphDirty();
}

void NodeFlowBase::setEdges(const QVariantList &edges)
{
    m_edges.clear();
    m_edges.reserve(edges.size());
    for (const QVariant &v : edges) {
        m_edges.push_back(edgeFromVariant(v.toMap()));
    }
    m_selectedEdgeIndex = -1;
    m_undoStack.clear();
    emitGraphChanged();
    markGraphDirty();
}

int NodeFlowBase::addNodeInternal(const QString &title, const QString &subtitle, const QColor &color, const QPointF &position)
{
    Node node;
    node.id = m_nextNodeId++;
    node.title = title;
    node.subtitle = subtitle;
    node.color = color.isValid() ? color : statusColor(node.id);
    node.rect = QRectF(position, QSizeF(NodeWidth, NodeHeight));
    m_nodes.push_back(node);
    setSelectedNode(node.id);
    emitGraphChanged();
    markGraphDirty();
    return node.id;
}

void NodeFlowBase::removeNodeInternal(int id)
{
    m_nodes.erase(std::remove_if(m_nodes.begin(), m_nodes.end(), [id](const Node &n) { return n.id == id; }), m_nodes.end());
    m_edges.erase(std::remove_if(m_edges.begin(), m_edges.end(), [id](const Edge &e) { return e.from == id || e.to == id; }), m_edges.end());
    if (m_selectedNodeId == id) setSelectedNode(-1);
    emitGraphChanged();
    markGraphDirty();
}

void NodeFlowBase::restoreNodeInternal(const Node &node, const QList<Edge> &edges)
{
    m_nodes.push_back(node);
    for (const auto &e : edges) {
        m_edges.push_back(e);
    }
    m_nextNodeId = std::max(m_nextNodeId, node.id + 1);
    setSelectedNode(node.id);
    emitGraphChanged();
    markGraphDirty();
}

int NodeFlowBase::addEdgeInternal(int from, int to, const QString &label)
{
    m_edges.push_back({from, to, label});
    int index = static_cast<int>(m_edges.size()) - 1;
    setSelectedEdge(index);
    emitGraphChanged();
    markGraphDirty();
    return index;
}

void NodeFlowBase::removeEdgeInternal(int index)
{
    if (index < 0 || index >= m_edges.size()) return;
    m_edges.removeAt(index);
    if (m_selectedEdgeIndex == index) setSelectedEdge(-1);
    emitGraphChanged();
    markGraphDirty();
}

void NodeFlowBase::restoreEdgeInternal(const Edge &edge, int index)
{
    if (index >= 0 && index <= m_edges.size())
        m_edges.insert(index, edge);
    else
        m_edges.push_back(edge);
    setSelectedEdge(index);
    emitGraphChanged();
    markStateDirty();
}

void NodeFlowBase::setNodePositionInternal(int id, const QPointF &pos)
{
    if (Node *n = findNode(id)) {
        n->rect.moveTopLeft(pos);
        emit nodeSelected(n->id, n->title);
        markStateDirty();
    }
}

int NodeFlowBase::addNode(const QString &title, const QString &subtitle, const QColor &color, const QPointF &position)
{
    auto *cmd = new AddNodeCommand(this, title, subtitle, color, position);
    m_undoStack.push(cmd);
    return static_cast<AddNodeCommand*>(cmd)->nodeId();
}

void NodeFlowBase::addEdge(int from, int to, const QString &label)
{
    if (from == to || !findNode(from) || !findNode(to)) return;
    for (const Edge &edge : m_edges) {
        if (edge.from == from && edge.to == to) return;
    }
    m_undoStack.push(new AddEdgeCommand(this, from, to, label));
}

void NodeFlowBase::removeSelectedNode()
{
    if (m_selectedNodeId < 0) return;
    const Node* n = findNode(m_selectedNodeId);
    if (!n) return;

    QList<Edge> removedEdges;
    for (const Edge& e : m_edges) {
        if (e.from == m_selectedNodeId || e.to == m_selectedNodeId) {
            removedEdges.push_back(e);
        }
    }
    m_undoStack.push(new RemoveNodeCommand(this, *n, removedEdges));
}

void NodeFlowBase::removeSelectedEdge()
{
    if (m_selectedEdgeIndex < 0 || m_selectedEdgeIndex >= m_edges.size()) return;
    Edge e = m_edges.at(m_selectedEdgeIndex);
    m_undoStack.push(new RemoveEdgeCommand(this, e, m_selectedEdgeIndex));
}

void NodeFlowBase::removeEdgesOfSelectedNode()
{
    if (m_selectedNodeId < 0) return;
    const int id = m_selectedNodeId;
    bool changed = false;
    for (int i = m_edges.size() - 1; i >= 0; --i) {
        if (m_edges[i].from == id || m_edges[i].to == id) {
            m_undoStack.push(new RemoveEdgeCommand(this, m_edges[i], i));
            changed = true;
        }
    }
    if (changed) emitGraphChanged();
}

void NodeFlowBase::clearGraph()
{
    m_nodes.clear();
    m_edges.clear();
    m_selectedNodeId = -1;
    m_selectedEdgeIndex = -1;
    m_nextNodeId = 1;
    m_activeStep = 0;
    m_undoStack.clear();
    emitGraphChanged();
    emit nodeSelected(-1, QString());
    emit edgeSelected(-1, -1, QString());
    emit edgeIndexSelected(-1);
    markGraphDirty();
}

void NodeFlowBase::setNodeTitle(int id, const QString &title)
{
    Node *n = findNode(id);
    if (!n || title.trimmed().isEmpty()) return;
    n->title = title.trimmed();
    emit nodeSelected(n->id, n->title);
    markStateDirty();
}

void NodeFlowBase::setEdgeLabel(int index, const QString &label)
{
    if (index < 0 || index >= m_edges.size()) return;
    m_edges[index].label = label.trimmed();
    emit edgeSelected(m_edges[index].from, m_edges[index].to, m_edges[index].label);
    markStateDirty();
}

QVariantList NodeFlowBase::nodes() const
{
    QVariantList list;
    list.reserve(m_nodes.size());
    for (const Node &node : m_nodes) list.push_back(nodeToVariant(node));
    return list;
}

QVariantList NodeFlowBase::edges() const
{
    QVariantList list;
    list.reserve(m_edges.size());
    for (const Edge &edge : m_edges) list.push_back(edgeToVariant(edge));
    return list;
}

QVariantMap NodeFlowBase::node(int id) const
{
    const Node *n = findNode(id);
    return n ? nodeToVariant(*n) : QVariantMap();
}

QVariantMap NodeFlowBase::edge(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_edges.size())) return QVariantMap();
    return edgeToVariant(m_edges.at(index));
}

QString NodeFlowBase::nodeTitle(int id) const
{
    const Node *n = findNode(id);
    return n ? n->title : QString();
}

QString NodeFlowBase::edgeLabel(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_edges.size())) return QString();
    return m_edges.at(index).label;
}

void NodeFlowBase::removeSelectedItem()
{
    if (m_selectedEdgeIndex >= 0) {
        removeSelectedEdge();
        return;
    }
    removeSelectedNode();
}

void NodeFlowBase::undo() { m_undoStack.undo(); }
void NodeFlowBase::redo() { m_undoStack.redo(); }

void NodeFlowBase::fitToView()
{
    const QRectF bounds = graphBounds();
    if (bounds.isNull() || width() <= 0 || height() <= 0) return;

    const double xScale = (width() - 96.0) / std::max(1.0, bounds.width());
    const double yScale = (height() - 96.0) / std::max(1.0, bounds.height());
    applyZoom(std::min(xScale, yScale));
    const QPointF graphCenter = bounds.center();
    const QPointF widgetCenter(width() / 2.0, height() / 2.0);
    m_panOffset = widgetCenter - graphCenter * m_zoom;
    m_needInitialFit = false;
    markGraphDirty();
}

void NodeFlowBase::zoomIn() { applyZoom(m_zoom * 1.18); }
void NodeFlowBase::zoomOut() { applyZoom(m_zoom / 1.18); }

void NodeFlowBase::resetView()
{
    applyZoom(1.0);
    m_panOffset = QPointF(40.0, 40.0);
    markGraphDirty();
}

bool NodeFlowBase::exportJson(const QString &fileName) const
{
    QJsonArray nodes;
    for (const Node &node : m_nodes) {
        QJsonObject item;
        item.insert(QStringLiteral("id"), node.id);
        item.insert(QStringLiteral("title"), node.title);
        item.insert(QStringLiteral("subtitle"), node.subtitle);
        item.insert(QStringLiteral("color"), node.color.name(QColor::HexRgb).toUpper());
        item.insert(QStringLiteral("x"), node.rect.x());
        item.insert(QStringLiteral("y"), node.rect.y());
        item.insert(QStringLiteral("width"), node.rect.width());
        item.insert(QStringLiteral("height"), node.rect.height());
        nodes.append(item);
    }

    QJsonArray edges;
    for (const Edge &edge : m_edges) {
        QJsonObject item;
        item.insert(QStringLiteral("from"), edge.from);
        item.insert(QStringLiteral("to"), edge.to);
        item.insert(QStringLiteral("label"), edge.label);
        edges.append(item);
    }

    QJsonObject root;
    root.insert(QStringLiteral("nodes"), nodes);
    root.insert(QStringLiteral("edges"), edges);

    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

bool NodeFlowBase::importJson(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) return false;

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;

    const QJsonObject root = doc.object();
    QVariantList nodeList;
    for (const QJsonValue &value : root.value(QStringLiteral("nodes")).toArray()) {
        nodeList.push_back(value.toObject().toVariantMap());
    }
    QVariantList edgeList;
    for (const QJsonValue &value : root.value(QStringLiteral("edges")).toArray()) {
        edgeList.push_back(value.toObject().toVariantMap());
    }

    setNodes(nodeList);
    setEdges(edgeList);
    return true;
}

void NodeFlowBase::beginConnectionFrom(int nodeId)
{
    if (const Node *n = findNode(nodeId)) {
        m_connecting = true;
        m_connectionFromId = nodeId;
        m_connectionEnd = nodeAnchor(*n, true);
        setSelectedNode(nodeId);
    }
}

QPointF NodeFlowBase::sceneToItem(const QPointF &scenePoint) const { return sceneToWidget(scenePoint); }
QPointF NodeFlowBase::itemToScene(const QPointF &itemPoint) const { return widgetToScene(itemPoint); }

void NodeFlowBase::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    JASPControl::geometryChange(newGeometry, oldGeometry);
    if (m_needInitialFit && !m_nodes.isEmpty() && width() > 0 && height() > 0) {
        QTimer::singleShot(0, this, &NodeFlowBase::fitToView);
    }
    markGridDirty();
}

// ============================================================================
// Event Handling
// ============================================================================

void NodeFlowBase::mouseDoubleClickEvent(QMouseEvent *event)
{
    const QPointF scenePoint = widgetToScene(event->position());
    const int nodeId = hitNode(scenePoint);
    if (nodeId >= 0) {
        setSelectedNode(nodeId);
        if (const Node *n = findNode(nodeId)) emit nodeTitleEditRequested(nodeId, n->title);
        return;
    }

    const int edgeIndex = hitEdge(scenePoint);
    if (edgeIndex >= 0) {
        setSelectedEdge(edgeIndex);
        emit edgeLabelEditRequested(edgeIndex, m_edges.at(edgeIndex).label);
        return;
    }
    JASPControl::mouseDoubleClickEvent(event);
}

void NodeFlowBase::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus();
    m_lastWidgetPos = event->position();
    m_lastScenePos = widgetToScene(event->position());

    if (event->button() == Qt::RightButton) {
        const int nodeId = hitNode(m_lastScenePos);
        const int edgeIndex = nodeId < 0 ? hitEdge(m_lastScenePos) : -1;
        if (nodeId >= 0) setSelectedNode(nodeId);
        else if (edgeIndex >= 0) setSelectedEdge(edgeIndex);
        emit contextMenuRequested(event->position().x(), event->position().y(),
                                  nodeId, edgeIndex, m_lastScenePos.x(), m_lastScenePos.y());
        return;
    }

    if (event->button() == Qt::MiddleButton || event->modifiers().testFlag(Qt::AltModifier)) {
        m_panning = true;
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    if (event->button() == Qt::LeftButton) {
        const int id = hitNode(m_lastScenePos);

        if (m_connectionMode) {
            if (id < 0) {
                m_connecting = false;
                m_connectionFromId = -1;
                markStateDirty();
                return;
            }
            if (!m_connecting) {
                setSelectedNode(id);
                m_connecting = true;
                m_connectionFromId = id;
                m_connectionEnd = m_lastScenePos;
                markStateDirty();
                return;
            }
            if (id != m_connectionFromId) {
                addEdge(m_connectionFromId, id, QStringLiteral("next"));
                setSelectedNode(id);
            }
            m_connecting = false;
            m_connectionFromId = -1;
            markStateDirty();
            return;
        }

        if (id >= 0) {
            setSelectedNode(id);
            m_draggingNode = true;
            if (Node* n = findNode(id)) m_dragStartPos = n->rect.topLeft();
        } else {
            const int edgeIndex = hitEdge(m_lastScenePos);
            setSelectedEdge(edgeIndex);
            m_draggingNode = false;
        }

        if (event->modifiers().testFlag(Qt::ShiftModifier) && id >= 0) {
            m_connecting = true;
            m_connectionFromId = id;
            m_connectionEnd = m_lastScenePos;
            m_draggingNode = false;
        }
    }
}

void NodeFlowBase::mouseMoveEvent(QMouseEvent *event)
{
    updatePointerScenePosition(event->position());
}

void NodeFlowBase::hoverMoveEvent(QHoverEvent *event)
{
    if (m_connecting) updatePointerScenePosition(event->position());
    JASPControl::hoverMoveEvent(event);
}

void NodeFlowBase::updatePointerScenePosition(const QPointF &itemPos)
{
    const QPointF scenePos = widgetToScene(itemPos);

    if (m_panning) {
        m_panOffset += itemPos - m_lastWidgetPos;
        m_lastWidgetPos = itemPos;
        markGridDirty(); // Only grid needs updating when panning!
        return;
    }

    if (m_connecting) {
        m_connectionEnd = scenePos;
        markStateDirty();
        return;
    }

    if (m_draggingNode && m_selectedNodeId >= 0) {
        const QPointF delta = scenePos - m_lastScenePos;
        if (Node *n = findNode(m_selectedNodeId)) {
            n->rect.translate(delta);
            emit nodeSelected(n->id, n->title);
        }
        m_lastScenePos = scenePos;
        markStateDirty();
    }
}

void NodeFlowBase::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton || m_panning) {
        m_panning = false;
        unsetCursor();
    }

    if (event->button() == Qt::LeftButton && m_draggingNode && m_selectedNodeId >= 0) {
        if (Node *n = findNode(m_selectedNodeId)) {
            if (n->rect.topLeft() != m_dragStartPos) {
                m_undoStack.push(new MoveNodeCommand(this, m_selectedNodeId, m_dragStartPos, n->rect.topLeft()));
            }
        }
    }

    if (event->button() == Qt::LeftButton && m_connecting && !m_connectionMode) {
        const int target = hitNode(widgetToScene(event->position()));
        if (target >= 0 && target != m_connectionFromId) {
            addEdge(m_connectionFromId, target, QStringLiteral("next"));
        }
        m_connecting = false;
        m_connectionFromId = -1;
        markStateDirty();
    }

    m_draggingNode = false;
}

void NodeFlowBase::wheelEvent(QWheelEvent *event)
{
    const QPointF scenePoint = widgetToScene(event->position());
    const double factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    applyZoom(m_zoom * factor);
    m_panOffset = event->position() - scenePoint * m_zoom;
    markGraphDirty();
}

void NodeFlowBase::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        removeSelectedItem();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        m_connecting = false;
        m_connectionFromId = -1;
        markStateDirty();
        return;
    }
    if (event->matches(QKeySequence::Undo)) {
        undo();
        return;
    }
    if (event->matches(QKeySequence::Redo)) {
        redo();
        return;
    }
    JASPControl::keyPressEvent(event);
}

// ============================================================================
// Internal Helpers
// ============================================================================

QRectF NodeFlowBase::nodeRect(const Node &node) const { return node.rect.normalized(); }

QPointF NodeFlowBase::sceneToWidget(const QPointF &point) const { return point * m_zoom + m_panOffset; }
QPointF NodeFlowBase::widgetToScene(const QPointF &point) const { return (point - m_panOffset) / std::max(0.001, m_zoom); }
QRectF NodeFlowBase::sceneToWidget(const QRectF &rect) const { return QRectF(sceneToWidget(rect.topLeft()), sceneToWidget(rect.bottomRight())).normalized(); }

int NodeFlowBase::hitNode(const QPointF &scenePoint) const
{
    for (int i = static_cast<int>(m_nodes.size()) - 1; i >= 0; --i) {
        if (nodeRect(m_nodes.at(i)).contains(scenePoint)) return m_nodes.at(i).id;
    }
    return -1;
}

int NodeFlowBase::hitEdge(const QPointF &scenePoint) const
{
    QPainterPathStroker stroker;
    stroker.setWidth(std::max(8.0, 12.0 / std::max(0.25, m_zoom)));
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);

    for (int i = static_cast<int>(m_edges.size()) - 1; i >= 0; --i) {
        const QPainterPath stroke = stroker.createStroke(edgePath(m_edges.at(i)));
        if (stroke.contains(scenePoint)) return i;
    }
    return -1;
}

NodeFlowBase::Node *NodeFlowBase::findNode(int id)
{
    for (Node &node : m_nodes) if (node.id == id) return &node;
    return nullptr;
}

const NodeFlowBase::Node *NodeFlowBase::findNode(int id) const
{
    for (const Node &node : m_nodes) if (node.id == id) return &node;
    return nullptr;
}

QRectF NodeFlowBase::graphBounds() const
{
    if (m_nodes.isEmpty()) return QRectF();
    QRectF bounds;
    bool initialized = false;
    for (const Node &node : m_nodes) {
        bounds = initialized ? bounds.united(nodeRect(node)) : nodeRect(node);
        initialized = true;
    }
    return bounds.adjusted(-80, -60, 80, 60);
}

QPointF NodeFlowBase::nodeAnchor(const Node &node, bool output) const
{
    const QRectF rect = nodeRect(node);
    return output ? QPointF(rect.right(), rect.center().y()) : QPointF(rect.left(), rect.center().y());
}

QPainterPath NodeFlowBase::edgePath(const Edge &edge) const
{
    const Node *from = findNode(edge.from);
    const Node *to = findNode(edge.to);
    if (!from || !to) return {};

    const QPointF start = nodeAnchor(*from, true);
    const QPointF end = nodeAnchor(*to, false);
    const double handle = std::max(80.0, std::abs(end.x() - start.x()) * 0.45);

    QPainterPath path(start);
    path.cubicTo(QPointF(start.x() + handle, start.y()),
                 QPointF(end.x() - handle, end.y()),
                 end);
    return path;
}

void NodeFlowBase::setSelectedNode(int id)
{
    m_selectedNodeId = id;
    m_selectedEdgeIndex = -1;
    emit edgeSelected(-1, -1, QString());
    emit edgeIndexSelected(-1);
    if (const Node *n = findNode(id)) emit nodeSelected(n->id, n->title);
    else emit nodeSelected(-1, QString());
    markStateDirty();
}

void NodeFlowBase::setSelectedEdge(int index)
{
    m_selectedEdgeIndex = index;
    m_selectedNodeId = -1;
    emit nodeSelected(-1, QString());

    if (index >= 0 && index < static_cast<int>(m_edges.size())) {
        const Edge &edge = m_edges.at(index);
        emit edgeSelected(edge.from, edge.to, edge.label);
    } else {
        emit edgeSelected(-1, -1, QString());
    }
    emit edgeIndexSelected(m_selectedEdgeIndex);
    markStateDirty();
}

void NodeFlowBase::emitGraphChanged() { emit graphChanged(nodeCount(), edgeCount()); }

void NodeFlowBase::applyZoom(double newZoom)
{
    const double clamped = std::clamp(newZoom, MinZoom, MaxZoom);
    if (!qFuzzyCompare(clamped + 1.0, m_zoom + 1.0)) {
        m_zoom = clamped;
        emit zoomChanged(m_zoom);
    } else {
        m_zoom = clamped;
    }
}