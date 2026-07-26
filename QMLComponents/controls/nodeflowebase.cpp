#include "nodeflowebase.h"

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
#include <QSGSimpleTextureNode> // Qt6 Scene Graph 支持
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
    case 1: return QColor("#1F9D63");
    case 2: return QColor("#D87516");
    case 3: return QColor("#7A5CFA");
    case 4: return QColor("#E9573F");
    default: return QColor("#2E7DD1");
    }
}
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
        markStateDirty(); // 使用 markStateDirty 而不是 m_transformOnly
        update();
    });
    timer->start();

    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *win) {
        if (win) m_dpr = win->effectiveDevicePixelRatio();
    });
}

void NodeFlowBase::markGraphDirty() {
    m_graphDirty = true;
    m_stateDirty = false;
    m_transformOnly = false;
    update();
}

void NodeFlowBase::markStateDirty() {
    if (!m_graphDirty) {
        m_stateDirty = true;
        m_transformOnly = false;
        update();
    }
}
// NodeFlowBase::NodeFlowBase(QQuickItem *parent)
//     : JASPControl(parent) // 构造函数初始化基类
// {
//     setFlag(ItemHasContents, true);  // 关键：启用 Scene Graph 自定义绘制
//     // JASPControl 默认可能不接收交互，这里需要手动开启
//     setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
//     setAcceptHoverEvents(true);
//     setFlag(ItemAcceptsDrops, false); // 根据需求调整
//     setImplicitWidth(660);
//     setImplicitHeight(460);

//     // 原有的动画定时器逻辑保持不变
//     auto *timer = new QTimer(this);
//     timer->setInterval(850);
//     connect(timer, &QTimer::timeout, this, [this]() {
//         if (!m_running || m_nodes.isEmpty()) return;
//         const int count = std::max(1, static_cast<int>(m_nodes.size()));
//         m_activeStep = (m_activeStep + 1) % count;
//         update(); // 触发 updatePaintNode
//     });
//     timer->start();
// }

// QSGNode *NodeFlowBase::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
// {
//     QSGImageNode *node = static_cast<QSGImageNode *>(oldNode);

//     if (width() <= 0 || height() <= 0) {
//         if (node) node->setTexture(nullptr);
//         return node;
//     }

//     if (!node) {
//         node = window()->createImageNode();
//         node->setFiltering(QSGTexture::Linear);
//     }

//     // ⭐ 取得屏幕的物理像素比
//     const qreal dpr = window()->effectiveDevicePixelRatio();

//     // 逻辑大小仍是 width() x height()
//     QImage image(QSize(width(), height()), QImage::Format_ARGB32_Premultiplied);
//     image.fill(Qt::transparent);

//     // ⭐ 设置图像物理像素密度，这样 QPainter 会自动绘制到高分辨率缓冲区
//     image.setDevicePixelRatio(dpr);

//     {
//         QPainter painter(&image);
//         painter.setRenderHint(QPainter::Antialiasing, true);
//         drawContent(&painter);
//     }

//     QSGTexture *texture = window()->createTextureFromImage(image, QQuickWindow::TextureHasAlphaChannel);
//     if (texture) {
//         node->setTexture(texture);
//         node->setRect(boundingRect());   // 将高分辨率纹理缩小到 Item 显示范围
//     } else {
//         node->setTexture(nullptr);
//     }

//     return node;
// }


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
    }

    // If graph structure changed, discard old tree
    if (m_graphDirty && oldNode) {
        delete oldNode;
        oldNode = nullptr;
        cleanupSceneGraphPointers();
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

        // New root needs full rebuild
        m_graphDirty = true;
    }

    // Update transform
    QMatrix4x4 matrix;
    matrix.translate(m_panOffset.x(), m_panOffset.y());
    matrix.scale(m_zoom);
    root->setMatrix(matrix);

    // Rebuild grid if zoom changed or full rebuild
    static double s_lastZoom = -1.0;
    if (m_graphDirty || !qFuzzyCompare(s_lastZoom, m_zoom)) {
        rebuildGridGraphics();
        s_lastZoom = m_zoom;
    }

    if (m_graphDirty) {
        rebuildEdgeGraphics();
        rebuildNodeGraphics();
        rebuildDraftEdgeGraphics();
        m_graphDirty = false;
        m_stateDirty = false;
    } else if (m_stateDirty) {
        rebuildEdgeGraphics();
        rebuildNodeGraphics();
        rebuildDraftEdgeGraphics();
        m_stateDirty = false;
    } else if (m_transformOnly) {
        m_transformOnly = false;
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

    const double drawStep = 32.0 * m_zoom;
    if (drawStep < 8.0) return;

    int imgW = static_cast<int>(std::ceil(width() * m_dpr));
    int imgH = static_cast<int>(std::ceil(height() * m_dpr));
    if (imgW <= 0 || imgH <= 0) return;

    QImage gridImage(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
    gridImage.fill(Qt::transparent);
    gridImage.setDevicePixelRatio(m_dpr);

    QPainter p(&gridImage);
    p.setPen(QPen(QColor("#E5EBF0"), 1));
    double startX = std::fmod(m_panOffset.x(), drawStep);
    double startY = std::fmod(m_panOffset.y(), drawStep);
    for (double x = startX; x < width(); x += drawStep)
        p.drawLine(QPointF(x, 0), QPointF(x, height()));
    for (double y = startY; y < height(); y += drawStep)
        p.drawLine(QPointF(0, y), QPointF(width(), y));
    p.end();

    auto *tex = window()->createTextureFromImage(gridImage, QQuickWindow::TextureHasAlphaChannel);
    if (!tex) return;

    auto *node = window()->createImageNode();
    node->setTexture(tex);
    node->setRect(boundingRect());
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

// ============================================================================
// 绘制逻辑主体
// ============================================================================
// void NodeFlowBase::drawContent(QPainter *painter)
// {
//     // 复用原 paint 的所有逻辑，painter 已由 updatePaintNode 准备好
//     painter->fillRect(boundingRect(), QColor("#F5F7FA"));

//     if (m_gridVisible) {
//         drawGrid(painter);
//     }

//     if (m_needInitialFit && !m_nodes.isEmpty()) {
//         // 注意：fitToView 内部调用了 update()，这在 updatePaintNode 中是安全的
//         // 但为避免递归，建议逻辑上小心。这里保持原样。
//         fitToView();
//     }

//     for (int i = 0; i < static_cast<int>(m_edges.size()); ++i) {
//         drawEdge(painter,
//                  m_edges.at(i),
//                  m_running && i == m_activeStep % std::max(1, edgeCount()),
//                  i == m_selectedEdgeIndex);
//     }
//     drawConnectionDraft(painter);

//     for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i) {
//         const Node &node = m_nodes.at(i);
//         drawNode(painter,
//                  node,
//                  node.id == m_selectedNodeId,
//                  m_running && i == m_activeStep % std::max(1, nodeCount()));
//     }
// }

//===========================================================================

// QSGNode* NodeFlowBase::createGridGraphics()
// {
//     auto *group = new QSGNode;
//     const double step = 32.0 * m_zoom;
//     if (step < 8.0) return group;

//     QSGGeometry *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0);
//     geometry->setDrawingMode(GL_LINES);
//     // 计算线的数量并分配顶点（略，实际需动态计算）
//     // 这里仅示意，完整实现需计算水平和垂直线段的起点终点
//     // 为简化，仍可使用一个 QSGImageNode 绘制网格（网格不常变，纹理方式也接受）
//     // 但为了完整性，下面给出纹理方式：
//     QImage gridImage(QSize(width(), height()), QImage::Format_ARGB32_Premultiplied);
//     gridImage.fill(Qt::transparent);
//     gridImage.setDevicePixelRatio(m_dpr);
//     QPainter p(&gridImage);
//     p.setPen(QPen(QColor("#E5EBF0"), 1));
//     double startX = std::fmod(m_panOffset.x(), step);
//     double startY = std::fmod(m_panOffset.y(), step);
//     for (double x = startX; x < width(); x += step)
//         p.drawLine(QPointF(x, 0), QPointF(x, height()));
//     for (double y = startY; y < height(); y += step)
//         p.drawLine(QPointF(0, y), QPointF(width(), y));
//     p.end();

//     auto *tex = window()->createTextureFromImage(gridImage, QQuickWindow::TextureHasAlphaChannel);
//     auto *node = window()->createImageNode();
//     node->setTexture(tex);
//     node->setRect(boundingRect());
//     node->setFiltering(QSGTexture::Linear);
//     group->appendChildNode(node);
//     return group;
// }

// QSGNode* NodeFlowBase::createNodeGraphics(const Node &node, bool selected, bool active)
// {
//     auto *group = new QSGNode;
//     QRectF rect = nodeRect(node);  // 场景坐标矩形

//     // 节点背景（圆角矩形）：使用一个 QSGImageNode 绘制固定大小的位图，然后通过变换放置
//     // 或者直接用 QSGGeometryNode 绘制圆角矩形（更复杂，这里用纹理方式快速过渡）
//     QImage img(NodeWidth * m_dpr, NodeHeight * m_dpr, QImage::Format_ARGB32_Premultiplied);
//     img.setDevicePixelRatio(m_dpr);
//     img.fill(Qt::transparent);
//     QPainter p(&img);
//     p.setRenderHint(QPainter::Antialiasing);
//     QColor border = selected ? QColor("#111827") : node.color.darker(115);
//     if (active) border = QColor("#E9573F");
//     p.setPen(QPen(border, selected || active ? 3 : 2));
//     p.setBrush(Qt::white);
//     p.drawRoundedRect(QRectF(0,0,NodeWidth,NodeHeight), 8, 8);
//     // 左侧色条
//     p.setPen(Qt::NoPen);
//     p.setBrush(node.color);
//     p.drawRoundedRect(QRectF(0,0,9,NodeHeight), 4, 4);
//     p.end();

//     auto *bgNode = window()->createImageNode();
//     bgNode->setTexture(window()->createTextureFromImage(img, QQuickWindow::TextureHasAlphaChannel));
//     bgNode->setRect(rect);   // 直接放在场景坐标处（根变换会处理）
//     bgNode->setFiltering(QSGTexture::Linear);
//     group->appendChildNode(bgNode);

//     // 文字标题纹理（独立分辨率管理）
//     auto *textNode = window()->createImageNode();
//     updateNodeTextTexture(node.id, textNode);   // 根据 zoom 生成清晰文字
//     // 文字定位（相对于节点背景）
//     QPointF textPos = rect.topLeft() + QPointF(20, 13);
//     textNode->setRect(QRectF(textPos, QSizeF(rect.width()-54, 24)));
//     group->appendChildNode(textNode);
//     m_nodeTextNodes[node.id] = textNode; // 保存引用

//     // 副标题同理，可再加一个 textNode2
//     // ...

//     // 状态圆点
//     QRectF dotRect(rect.right() - 26, rect.top() + 14, 12, 12);
//     auto *dotNode = window()->createImageNode();
//     QImage dotImg(12 * m_dpr, 12 * m_dpr, QImage::Format_ARGB32_Premultiplied);
//     dotImg.setDevicePixelRatio(m_dpr);
//     dotImg.fill(Qt::transparent);
//     QPainter dp(&dotImg);
//     dp.setRenderHint(QPainter::Antialiasing);
//     dp.setBrush(active ? QColor("#E9573F") : node.color.lighter(120));
//     dp.setPen(Qt::NoPen);
//     dp.drawEllipse(0,0,12,12);
//     dp.end();
//     dotNode->setTexture(window()->createTextureFromImage(dotImg, QQuickWindow::TextureHasAlphaChannel));
//     dotNode->setRect(dotRect);
//     group->appendChildNode(dotNode);

//     // 端口点（输入/输出）
//     // 同样使用小纹理或圆点几何
//     return group;
// }

QSGNode* NodeFlowBase::createNodeGraphics(const Node &node, bool selected, bool active)
{
    if (!window()) return nullptr;

    auto *group = new QSGNode();
    QRectF rect = nodeRect(node);

    // Background
    int bgW = static_cast<int>(std::ceil(NodeWidth * m_dpr));
    int bgH = static_cast<int>(std::ceil(NodeHeight * m_dpr));
    QImage bgImg(bgW, bgH, QImage::Format_ARGB32_Premultiplied);
    bgImg.setDevicePixelRatio(m_dpr);
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

    // Title
    QFont titleFont;
    titleFont.setPointSizeF(11.0);
    titleFont.setBold(true);
    auto *titleNode = createTextNode(node.title, titleFont, QColor("#1F2933"),
                                     QRectF(rect.topLeft() + QPointF(20, 13), QSizeF(NodeWidth - 54, 24)));
    if (titleNode) group->appendChildNode(titleNode);

    // Subtitle
    if (!node.subtitle.isEmpty()) {
        QFont subFont;
        subFont.setPointSizeF(9.0);
        auto *subNode = createTextNode(node.subtitle, subFont, QColor("#66727C"),
                                       QRectF(rect.topLeft() + QPointF(20, 38), QSizeF(NodeWidth - 32, 30)));
        if (subNode) group->appendChildNode(subNode);
    }

    // Status dot
    int dotW = static_cast<int>(std::ceil(12 * m_dpr));
    QImage dotImg(dotW, dotW, QImage::Format_ARGB32_Premultiplied);
    dotImg.setDevicePixelRatio(m_dpr);
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

    // Ports
    auto addPort = [&](const QPointF &scenePos) {
        int pw = static_cast<int>(std::ceil(8 * m_dpr));
        QImage portImg(pw, pw, QImage::Format_ARGB32_Premultiplied);
        portImg.setDevicePixelRatio(m_dpr);
        portImg.fill(Qt::transparent);
        {
            QPainter p(&portImg);
            p.setRenderHint(QPainter::Antialiasing);
            p.setBrush(QColor("#CDD7DF"));
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

    qreal scaleFactor = std::max(1.0, 1.0 / m_zoom);
    int texW = static_cast<int>(std::ceil(rect.width() * scaleFactor * m_dpr));
    int texH = static_cast<int>(std::ceil(rect.height() * scaleFactor * m_dpr));
    if (texW <= 0 || texH <= 0) return nullptr;

    QImage img(texW, texH, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(m_dpr * scaleFactor);
    img.fill(Qt::transparent);

    {
        QPainter p(&img);
        p.setRenderHint(QPainter::TextAntialiasing);
        QFont scaledFont = font;
        scaledFont.setPointSizeF(font.pointSizeF() * scaleFactor);
        p.setFont(scaledFont);
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

// void NodeFlowBase::updateNodeTextTexture(int nodeId, QSGImageNode* textNode, bool isTitle)
// {
//     Node *n = findNode(nodeId);
//     if (!n) return;

//     const QString &text = isTitle ? n->title : n->subtitle;
//     const double baseFontSizePt = isTitle ? 11.0 : 9.0;

//     // 计算纹理分辨率放大因子：当 m_zoom < 1 时，需要更高的纹理像素密度
//     double scaleFactor = std::max(1.0, 1.0 / m_zoom);
//     // 纹理的物理像素尺寸
//     int texWidth = 200 * scaleFactor * m_dpr;
//     int texHeight = 30 * scaleFactor * m_dpr;

//     QImage textImg(texWidth, texHeight, QImage::Format_ARGB32_Premultiplied);
//     // 关键：设置 devicePixelRatio 为 m_dpr * scaleFactor
//     textImg.setDevicePixelRatio(m_dpr * scaleFactor);
//     textImg.fill(Qt::transparent);

//     {
//         QPainter p(&textImg);
//         p.setRenderHint(QPainter::TextAntialiasing);
//         QFont font;
//         font.setPointSizeF(baseFontSizePt * scaleFactor);  // 在纹理坐标系中放大字体
//         font.setBold(isTitle);
//         p.setFont(font);
//         p.setPen(QColor("#1F2933")); // 标题颜色
//         QRectF textRect(0, 0, texWidth / (m_dpr * scaleFactor), texHeight / (m_dpr * scaleFactor));
//         p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);
//     }

//     QSGTexture *tex = window()->createTextureFromImage(textImg, QQuickWindow::TextureHasAlphaChannel);
//     textNode->setTexture(tex);
//     textNode->setFiltering(QSGTexture::Linear);
//     // 设置纹理的显示矩形为逻辑大小（与 drawText 的矩形一致）
//     QSizeF logicalSize(texWidth / (m_dpr * scaleFactor), texHeight / (m_dpr * scaleFactor));
//     textNode->setRect(QRectF(QPointF(0,0), logicalSize));
// }

QSGNode* NodeFlowBase::createEdgeGraphics(const Edge &edge, bool selected, bool active)
{
    const Node *from = findNode(edge.from);
    const Node *to = findNode(edge.to);
    if (!from || !to || !window()) return new QSGNode();

    auto *group = new QSGNode();
    QPainterPath path = edgePath(edge);
    QRectF bounds = path.boundingRect().adjusted(-8, -8, 8, 8);
    if (bounds.width() <= 0 || bounds.height() <= 0) return group;

    int imgW = static_cast<int>(std::ceil(bounds.width() * m_dpr));
    int imgH = static_cast<int>(std::ceil(bounds.height() * m_dpr));
    if (imgW > 8192 || imgH > 8192) {
        // Too large, skip texture-based rendering for this edge
        return group;
    }

    QImage img(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(m_dpr);
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

    // Arrow
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

    // Label
    if (!edge.label.isEmpty()) {
        QPointF center = path.pointAtPercent(0.5);
        QRectF labelRect(center.x() - 36, center.y() - 12, 72, 24);

        // Label background
        int bgW = static_cast<int>(std::ceil(72 * m_dpr));
        int bgH = static_cast<int>(std::ceil(24 * m_dpr));
        QImage bgImg(bgW, bgH, QImage::Format_ARGB32_Premultiplied);
        bgImg.setDevicePixelRatio(m_dpr);
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

        // Label text
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

    int imgW = static_cast<int>(std::ceil(bounds.width() * m_dpr));
    int imgH = static_cast<int>(std::ceil(bounds.height() * m_dpr));
    QImage img(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(m_dpr);
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
    emitGraphChanged();
    markGraphDirty();
}

int NodeFlowBase::addNode(const QString &title, const QString &subtitle, const QColor &color, const QPointF &position)
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

void NodeFlowBase::addEdge(int from, int to, const QString &label)
{
    if (from == to || !findNode(from) || !findNode(to)) return;
    for (const Edge &edge : m_edges) {
        if (edge.from == from && edge.to == to) return;
    }
    m_edges.push_back({from, to, label});
    setSelectedEdge(static_cast<int>(m_edges.size()) - 1);
    emitGraphChanged();
    markGraphDirty();
}

void NodeFlowBase::removeSelectedNode()
{
    if (m_selectedNodeId < 0) return;
    const int id = m_selectedNodeId;
    m_nodes.erase(std::remove_if(m_nodes.begin(), m_nodes.end(), [id](const Node &n) { return n.id == id; }), m_nodes.end());
    m_edges.erase(std::remove_if(m_edges.begin(), m_edges.end(), [id](const Edge &e) { return e.from == id || e.to == id; }), m_edges.end());
    m_selectedEdgeIndex = -1;
    setSelectedNode(-1);
    emitGraphChanged();
    markGraphDirty();
}

void NodeFlowBase::removeSelectedEdge()
{
    if (m_selectedEdgeIndex < 0 || m_selectedEdgeIndex >= m_edges.size()) return;
    m_edges.removeAt(m_selectedEdgeIndex);
    setSelectedEdge(-1);
    emitGraphChanged();
    markGraphDirty();
}

void NodeFlowBase::removeEdgesOfSelectedNode()
{
    if (m_selectedNodeId < 0) return;
    const int id = m_selectedNodeId;
    const auto oldSize = m_edges.size();
    m_edges.erase(std::remove_if(m_edges.begin(), m_edges.end(), [id](const Edge &e) { return e.from == id || e.to == id; }), m_edges.end());
    if (m_edges.size() != oldSize) {
        m_selectedEdgeIndex = -1;
        emit edgeSelected(-1, -1, QString());
        emit edgeIndexSelected(-1);
        emitGraphChanged();
        markGraphDirty();
    }
}

void NodeFlowBase::clearGraph()
{
    m_nodes.clear();
    m_edges.clear();
    m_selectedNodeId = -1;
    m_selectedEdgeIndex = -1;
    m_nextNodeId = 1;
    m_activeStep = 0;
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
    markStateDirty(); // 只需要状态重建，结构没变
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
    for (const Node &node : m_nodes) {
        list.push_back(nodeToVariant(node));
    }
    return list;
}

QVariantList NodeFlowBase::edges() const
{
    QVariantList list;
    list.reserve(m_edges.size());
    for (const Edge &edge : m_edges) {
        list.push_back(edgeToVariant(edge));
    }
    return list;
}

QVariantMap NodeFlowBase::node(int id) const
{
    const Node *n = findNode(id);
    return n ? nodeToVariant(*n) : QVariantMap();
}

QVariantMap NodeFlowBase::edge(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_edges.size())) {
        return QVariantMap();
    }
    return edgeToVariant(m_edges.at(index));
}

QString NodeFlowBase::nodeTitle(int id) const
{
    const Node *n = findNode(id);
    return n ? n->title : QString();
}

QString NodeFlowBase::edgeLabel(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_edges.size())) {
        return QString();
    }
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
    m_transformOnly = true; // 变换变化
    update();
}

void NodeFlowBase::zoomIn()
{
    applyZoom(m_zoom * 1.18);
    update();
}

void NodeFlowBase::zoomOut()
{
    applyZoom(m_zoom / 1.18);
    update();
}

void NodeFlowBase::resetView()
{
    applyZoom(1.0);
    m_panOffset = QPointF(40.0, 40.0);
    update();
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
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

bool NodeFlowBase::importJson(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }

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
    const Node *n = findNode(nodeId);
    if (!n) {
        return;
    }
    // setConnectionMode(true);
    m_connecting = true;
    m_connectionFromId = nodeId;
    m_connectionEnd = nodeAnchor(*n, true);
    setSelectedNode(nodeId);
    update();
}

QPointF NodeFlowBase::sceneToItem(const QPointF &scenePoint) const
{
    return sceneToWidget(scenePoint);
}

QPointF NodeFlowBase::itemToScene(const QPointF &itemPoint) const
{
    return widgetToScene(itemPoint);
}

bool NodeFlowBase::gridVisible() const { return m_gridVisible; }
bool NodeFlowBase::isRunning() const { return m_running; }
bool NodeFlowBase::connectionMode() const { return m_connectionMode; }
double NodeFlowBase::zoom() const { return m_zoom; }
int NodeFlowBase::selectedNodeId() const { return m_selectedNodeId; }
int NodeFlowBase::selectedEdgeIndex() const { return m_selectedEdgeIndex; }
int NodeFlowBase::nodeCount() const { return static_cast<int>(m_nodes.size()); }
int NodeFlowBase::edgeCount() const { return static_cast<int>(m_edges.size()); }

void NodeFlowBase::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    JASPControl::geometryChange(newGeometry, oldGeometry);
    if (m_needInitialFit && !m_nodes.isEmpty() && width() > 0 && height() > 0) {
        // Use single shot to avoid recursion
        QTimer::singleShot(0, this, &NodeFlowBase::fitToView);
    }
}

// ============================================================================
// 事件处理实现
// ============================================================================

void NodeFlowBase::mouseDoubleClickEvent(QMouseEvent *event)
{
    const QPointF scenePoint = widgetToScene(event->position());
    const int nodeId = hitNode(scenePoint);
    if (nodeId >= 0) {
        setSelectedNode(nodeId);
        if (const Node *n = findNode(nodeId)) {
            emit nodeTitleEditRequested(nodeId, n->title);
        }
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
        if (nodeId >= 0) {
            setSelectedNode(nodeId);
        } else if (edgeIndex >= 0) {
            setSelectedEdge(edgeIndex);
        }
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
                update();
                return;
            }

            if (!m_connecting) {
                setSelectedNode(id);
                m_connecting = true;
                m_connectionFromId = id;
                m_connectionEnd = m_lastScenePos;
                update();
                return;
            }

            if (id != m_connectionFromId) {
                addEdge(m_connectionFromId, id, QStringLiteral("next"));
                setSelectedNode(id);
            }
            m_connecting = false;
            m_connectionFromId = -1;
            update();
            return;
        }

        if (id >= 0) {
            setSelectedNode(id);
            m_draggingNode = true;
        } else {
            const int edgeIndex = hitEdge(m_lastScenePos);
            setSelectedEdge(edgeIndex);
            m_draggingNode = false;
            return;
        }

        if (event->modifiers().testFlag(Qt::ShiftModifier)) {
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
    if (m_connecting) {
        updatePointerScenePosition(event->position());
    }
    JASPControl::hoverMoveEvent(event);
}

void NodeFlowBase::updatePointerScenePosition(const QPointF &itemPos)
{
    const QPointF scenePos = widgetToScene(itemPos);

    if (m_panning) {
        m_panOffset += itemPos - m_lastWidgetPos;
        m_lastWidgetPos = itemPos;
        m_transformOnly = true; // 只有变换变化
        update();
        return;
    }

    if (m_connecting) {
        m_connectionEnd = scenePos;
        m_transformOnly = true; // 草稿线位置变化，但结构没变
        update();
        return;
    }

    if (m_draggingNode && m_selectedNodeId >= 0) {
        const QPointF delta = scenePos - m_lastScenePos;
        if (Node *n = findNode(m_selectedNodeId)) {
            n->rect.translate(delta);
            emit nodeSelected(n->id, n->title);
        }
        m_lastScenePos = scenePos;
        markStateDirty(); // 节点位置变化，需要重绘
    }
}

void NodeFlowBase::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton || m_panning) {
        m_panning = false;
        unsetCursor();
    }

    if (event->button() == Qt::LeftButton && m_connecting && !m_connectionMode) {
        const int target = hitNode(widgetToScene(event->position()));
        if (target >= 0 && target != m_connectionFromId) {
            addEdge(m_connectionFromId, target, QStringLiteral("next"));
        }
        m_connecting = false;
        m_connectionFromId = -1;
        update();
    }

    m_draggingNode = false;
}

void NodeFlowBase::wheelEvent(QWheelEvent *event)
{
    const QPointF scenePoint = widgetToScene(event->position());
    const double factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    applyZoom(m_zoom * factor);
    m_panOffset = event->position() - scenePoint * m_zoom;
    m_transformOnly = true;
    update();
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
    JASPControl::keyPressEvent(event);
}

// ============================================================================
// 内部辅助函数 (保持不变)
// ============================================================================

QRectF NodeFlowBase::nodeRect(const Node &node) const
{
    return node.rect.normalized();
}

QPointF NodeFlowBase::sceneToWidget(const QPointF &point) const
{
    return point * m_zoom + m_panOffset;
}

QPointF NodeFlowBase::widgetToScene(const QPointF &point) const
{
    return (point - m_panOffset) / std::max(0.001, m_zoom);
}

QRectF NodeFlowBase::sceneToWidget(const QRectF &rect) const
{
    return QRectF(sceneToWidget(rect.topLeft()), sceneToWidget(rect.bottomRight())).normalized();
}

int NodeFlowBase::hitNode(const QPointF &scenePoint) const
{
    for (int i = static_cast<int>(m_nodes.size()) - 1; i >= 0; --i) {
        if (nodeRect(m_nodes.at(i)).contains(scenePoint)) {
            return m_nodes.at(i).id;
        }
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
        if (stroke.contains(scenePoint)) {
            return i;
        }
    }
    return -1;
}

NodeFlowBase::Node *NodeFlowBase::findNode(int id)
{
    for (Node &node : m_nodes) {
        if (node.id == id) {
            return &node;
        }
    }
    return nullptr;
}

const NodeFlowBase::Node *NodeFlowBase::findNode(int id) const
{
    for (const Node &node : m_nodes) {
        if (node.id == id) {
            return &node;
        }
    }
    return nullptr;
}

QRectF NodeFlowBase::graphBounds() const
{
    if (m_nodes.isEmpty()) {
        return QRectF();
    }

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
    if (!from || !to) {
        return {};
    }

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
    if (const Node *n = findNode(id)) {
        emit nodeSelected(n->id, n->title);
    } else {
        emit nodeSelected(-1, QString());
    }
    update();
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
    update();
}

void NodeFlowBase::emitGraphChanged()
{
    emit graphChanged(nodeCount(), edgeCount());
}

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

// void NodeFlowBase::drawGrid(QPainter *painter) const
// {
//     painter->save();
//     painter->setPen(QPen(QColor("#E5EBF0"), 1));
//     const double step = 32.0 * m_zoom;
//     if (step < 8.0) {
//         painter->restore();
//         return;
//     }

//     const double startX = std::fmod(m_panOffset.x(), step);
//     const double startY = std::fmod(m_panOffset.y(), step);
//     for (double x = startX; x < width(); x += step) {
//         painter->drawLine(QPointF(x, 0), QPointF(x, height()));
//     }
//     for (double y = startY; y < height(); y += step) {
//         painter->drawLine(QPointF(0, y), QPointF(width(), y));
//     }
//     painter->restore();
// }

// void NodeFlowBase::drawEdge(QPainter *painter, const Edge &edge, bool active, bool selected) const
// {
//     const Node *from = findNode(edge.from);
//     const Node *to = findNode(edge.to);
//     if (!from || !to) {
//         return;
//     }

//     QPainterPath path;
//     const QPainterPath scenePath = edgePath(edge);
//     for (int i = 0; i < scenePath.elementCount(); ++i) {
//         const QPainterPath::Element element = scenePath.elementAt(i);
//         if (element.isMoveTo()) {
//             path.moveTo(sceneToWidget(QPointF(element.x, element.y)));
//         } else if (element.isCurveTo()) {
//             const QPainterPath::Element c1 = scenePath.elementAt(i);
//             const QPainterPath::Element c2 = scenePath.elementAt(i + 1);
//             const QPainterPath::Element endElement = scenePath.elementAt(i + 2);
//             path.cubicTo(sceneToWidget(QPointF(c1.x, c1.y)),
//                          sceneToWidget(QPointF(c2.x, c2.y)),
//                          sceneToWidget(QPointF(endElement.x, endElement.y)));
//             i += 2;
//         }
//     }
//     const QPointF end = sceneToWidget(nodeAnchor(*to, false));

//     painter->save();
//     if (selected) {
//         painter->setPen(QPen(QColor("#111827"), 6));
//         painter->drawPath(path);
//     }

//     painter->setPen(QPen(active ? QColor("#E9573F") : QColor("#8EA0AD"), active || selected ? 4 : 2));
//     painter->setBrush(Qt::NoBrush);
//     painter->drawPath(path);

//     const QLineF tail(QPointF(end.x() - 16, end.y() - 7), end);
//     const QLineF head(QPointF(end.x() - 16, end.y() + 7), end);
//     painter->drawLine(tail);
//     painter->drawLine(head);

//     if (!edge.label.isEmpty()) {
//         const QPointF center = path.pointAtPercent(0.5);
//         const QRectF labelRect(center.x() - 36, center.y() - 12, 72, 24);
//         painter->setPen(Qt::NoPen);
//         painter->setBrush(active ? QColor("#FFE8E3") : QColor("#FFFFFF"));
//         painter->drawRoundedRect(labelRect, 4, 4);
//         painter->setPen(active ? QColor("#C7442F") : QColor("#66727C"));
//         painter->drawText(labelRect, Qt::AlignCenter, edge.label);
//     }
//     painter->restore();
// }

// void NodeFlowBase::drawNode(QPainter *painter, const Node &node, bool selected, bool active) const
// {
//     const QRectF rect = sceneToWidget(nodeRect(node));
//     painter->save();

//     QColor border = selected ? QColor("#111827") : node.color.darker(115);
//     if (active) {
//         border = QColor("#E9573F");
//     }

//     painter->setPen(QPen(border, selected || active ? 3 : 2));
//     painter->setBrush(QColor("#FFFFFF"));
//     painter->drawRoundedRect(rect, 8, 8);

//     const QRectF stripe(rect.left(), rect.top(), 9, rect.height());
//     painter->setPen(Qt::NoPen);
//     painter->setBrush(node.color);
//     painter->drawRoundedRect(stripe, 4, 4);

//     const QRectF dot(rect.right() - 26, rect.top() + 14, 12, 12);
//     painter->setBrush(active ? QColor("#E9573F") : node.color.lighter(120));
//     painter->drawEllipse(dot);

//     painter->setPen(QColor("#1F2933"));
//     QFont titleFont = painter->font();
//     titleFont.setPointSizeF(std::max(8.0, 11.0 * m_zoom));
//     titleFont.setBold(true);
//     painter->setFont(titleFont);
//     painter->drawText(rect.adjusted(20, 13, -34, -40), Qt::AlignLeft | Qt::AlignVCenter, node.title);

//     painter->setPen(QColor("#66727C"));
//     QFont subtitleFont = painter->font();
//     subtitleFont.setPointSizeF(std::max(7.0, 9.0 * m_zoom));
//     subtitleFont.setBold(false);
//     painter->setFont(subtitleFont);
//     painter->drawText(rect.adjusted(20, 38, -12, -10), Qt::AlignLeft | Qt::AlignVCenter, node.subtitle);

//     painter->setPen(QPen(QColor("#CDD7DF"), 1));
//     painter->setBrush(QColor("#FFFFFF"));
//     painter->drawEllipse(sceneToWidget(nodeAnchor(node, false)), 4, 4);
//     painter->drawEllipse(sceneToWidget(nodeAnchor(node, true)), 4, 4);
//     painter->restore();
// }

// void NodeFlowBase::drawConnectionDraft(QPainter *painter) const
// {
//     if (!m_connecting) {
//         return;
//     }

//     const Node *from = findNode(m_connectionFromId);
//     if (!from) {
//         return;
//     }

//     const QPointF start = sceneToWidget(nodeAnchor(*from, true));
//     const QPointF end = sceneToWidget(m_connectionEnd);
//     QPainterPath path(start);
//     const double handle = std::max(80.0, std::abs(end.x() - start.x()) * 0.45);
//     path.cubicTo(QPointF(start.x() + handle, start.y()),
//                  QPointF(end.x() - handle, end.y()),
//                  end);

//     painter->save();
//     painter->setPen(QPen(QColor("#2E7DD1"), 2, Qt::DashLine));
//     painter->setBrush(Qt::NoBrush);
//     painter->drawPath(path);
//     painter->restore();
// }
