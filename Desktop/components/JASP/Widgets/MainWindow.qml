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

import QtQuick
import QtQuick.Window
import JASP
import QtQuick.Controls
import JASP.Controls as JC

Window
{
    id:					mainWindowRoot
    title:				mainWindow.windowTitle
	visible:			true
	width:				1280
	height:				720
	flags:				Qt.Window | Qt.WindowFullscreenButtonHint
	color:				mainWindow.hadFatalError ? jaspTheme.red : jaspTheme.white
	minimumWidth:		jaspTheme.formWidth + 2 * jaspTheme.splitHandleWidth + jaspTheme.scrollbarBoxWidthBig + 3
	minimumHeight:		400 * jaspTheme.uiScale
	visibility:			!preferencesModel.startMaximized ? Window.Windowed : Window.Maximized

	onVisibleChanged:
		if(!visible)
		{
			helpModel.visible  = false;
			aboutModel.visible = false;
		}

	property real devicePixelRatio: Screen.devicePixelRatio

	readonly property string personaAvatar: preferencesModel.aiPersonaModel.activePersonaAvatar

	onDevicePixelRatioChanged: if(devicePixelRatio > 0) mainWindow.screenPPI = devicePixelRatio * 96

	onClosing: (close)=>
	{
		close.accepted = mainWindow.checkPackageModifiedBeforeClosing();

		if(close.accepted)
			mainWindow.closeWindows();
	}

	function toggleFullScreen()
	{
		mainWindowRoot.visibility = mainWindowRoot.visibility === Window.FullScreen ? Window.Windowed : Window.FullScreen;
	}

	function changeFocusToRibbon()
	{
		ribbon.focus = true;
 		ribbon.focusOnRibbonMenu();
	}

	function showWorkspaceMenu()
	{
		ribbonModel.showData()
		fileMenuModel.visible = false
		modulesMenu.opened	= true
	}

	function changeFocusToModulesMenu()
	{
		ribbon.showModulesMenuPressed();
	}

	function changeFocusToFileMenu()
	{
		ribbon.forceActiveFocus();
		ribbon.showFileMenuPressed();
	}

	function mod (a, n)
	{
		return (a + n) % n;
	}

	DropArea
	{
		id: drop
		enabled: true
		anchors.fill: parent
		onDropped: (drop) => mainWindow.openURLFile(drop.text)
	}

	Item
	{
		anchors.fill:	parent
		
		Rectangle
		{
			z:				1
			visible:		mainWindow.hadFatalError
			color:			jaspTheme.red
			opacity:		0.75
			anchors.fill:	parent
		}

		Shortcut { onActivated: mainWindow.showEnginesWindow();					sequences: ["Ctrl+Alt+Shift+E"];								context: Qt.ApplicationShortcut; }
		Shortcut { onActivated: mainWindow.saveKeyPressed();					sequences: ["Ctrl+S", Qt.Key_Save];								context: Qt.ApplicationShortcut; }
		Shortcut { onActivated: mainWindow.saveAsKeyPressed();					sequences: ["Ctrl+Shift+S", Qt.Key_SaveAs];						context: Qt.ApplicationShortcut; }
		Shortcut { onActivated: { ribbon.showFileMenuPressed(); mainWindow.openKeyPressed();}
																				sequences: ["Ctrl+O"];											context: Qt.ApplicationShortcut; }
		//This is now redo! Shortcut { onActivated: mainWindow.syncKeyPressed();					sequences: ["Ctrl+Y", Qt.Key_Reload];							context: Qt.ApplicationShortcut; }
		Shortcut { onActivated: mainWindow.zoomInKeyPressed();					sequences: [Qt.Key_ZoomIn, "Ctrl+Plus", "Ctrl+\+", "Ctrl+\="];	context: Qt.ApplicationShortcut; }
		Shortcut { onActivated: mainWindow.zoomOutKeyPressed();					sequences: [Qt.Key_ZoomOut, "Ctrl+Minus", "Ctrl+\-"];			context: Qt.ApplicationShortcut; }
		Shortcut { onActivated: mainWindow.zoomResetKeyPressed();				sequences: ["Ctrl+0", Qt.Key_Zoom];								context: Qt.ApplicationShortcut; }
		Shortcut { onActivated: mainWindow.refreshKeyPressed();					sequences: ["Ctrl+R", Qt.Key_Refresh];							context: Qt.ApplicationShortcut; }
		Shortcut { onActivated: mainWindowRoot.close();							sequences: ["Ctrl+Q", Qt.Key_Close];							context: Qt.ApplicationShortcut; }
		Shortcut { onActivated: fileMenuModel.close();							sequences: ["Ctrl+W"];											}
		Shortcut { onActivated: mainWindow.toggleChat();								sequences: ["Ctrl+J"];									enabled: preferencesModel.aiEnabled }
		Shortcut { onActivated: mainWindowRoot.toggleFullScreen();				sequences: ["Ctrl+M", Qt.Key_F11];								context: Qt.ApplicationShortcut; }
		Shortcut { onActivated: mainWindowRoot.changeFocusToFileMenu();			sequences: ["Home",   Qt.Key_Home, Qt.Key_Menu];				}
		Shortcut { onActivated: mainWindow.setLanguage(0);						sequences: ["Ctrl+1"];											context: Qt.ApplicationShortcut; }
		Shortcut { onActivated: mainWindow.setLanguage(1);						sequences: ["Ctrl+2"];											context: Qt.ApplicationShortcut; }
		Shortcut { onActivated: mainWindow.undo();								sequences: ["Ctrl+Z", Qt.Key_Undo];								context: Qt.ApplicationShortcut; }
		Shortcut { onActivated: mainWindow.redo();								sequences: ["Ctrl+Shift+Z", "Ctrl-Y", Qt.Key_Redo];				context: Qt.ApplicationShortcut; }

        Shortcut { onActivated: { dynamicModules.refreshDeveloperModule(false, true);}  sequences: ["Ctrl+Shift+U"];                     context: Qt.ApplicationShortcut; }
        Shortcut { onActivated: { dynamicModules.refreshDeveloperModule(true, false);}  sequences: ["Ctrl+Shift+R"];                     context: Qt.ApplicationShortcut; }
        Shortcut { onActivated: { dynamicModules.refreshDeveloperModule(true, true);}   sequences: ["Ctrl+Shift+D"];                     context: Qt.ApplicationShortcut; }


		RibbonBar
		{
			id:		ribbon
			z:		6
			focus:	true

			anchors
			{
				top:	parent.top
				left:	parent.left
				right:	parent.right
			}
		}

		CustomMenu
		{
			id:			customMenu
			z:			5
			
			function hideMenus() 
			{
				customMenu.hide();
				customSubMenu.hide();
			}
		}
		CustomMenu
		{
			id:			customSubMenu
			z:			6
			isSubMenu:	true
		}

		FileMenu
		{
			id:			filemenu
			z:			3

			anchors
			{
				top:	ribbon.bottom
				left:	parent.left
				bottom:	parent.bottom
			}

		}

		WelcomePage
		{
			id:			welcomePage
			z:			0
			visible:	mainWindow.welcomePageVisible

			anchors
			{
				top:	ribbon.bottom
				left:	parent.left
				right:	parent.right
				bottom:	parent.bottom
			}
		}


		MainPage
		{
			id:			mainpage
			z:			0
			visible:	!mainWindow.welcomePageVisible

			anchors
			{
				top:	ribbon.bottom
				left:	parent.left
				right:	parent.right
				bottom:	parent.bottom
			}
		}


		// ============================================================
		// NodeFlow 调试面板 - 在 MainPage 后插入
		// ============================================================
		Item {
			id: nodeFlowDebugPanel
			z: 1000  // 确保在其他组件之上
			visible: true  // 默认隐藏，通过按钮显示
			anchors.fill: parent

			// 背景遮罩
			Rectangle {
				anchors.fill: parent
				color: jaspTheme.white
				opacity: 0.95
			}

			// 主布局：左侧控制面板，右侧 NodeFlow 视图
			Row {
				anchors.fill: parent
				spacing: 10

				// 左侧控制面板
				Rectangle {
					width: 280
					height: parent.height
					color: jaspTheme.uiBackground
					border.color: jaspTheme.borderColor
					border.width: 1

					Column {
						anchors.fill: parent
						anchors.margins: 10
						spacing: 8

						Text {
							text: qsTr("NodeFlow 调试面板")
							font.bold: true
							font.pixelSize: 16
							color: jaspTheme.textEnabled
						}

						Text {
							text: qsTr("节点操作")
							font.bold: true
							color: jaspTheme.textEnabled
						}

						// 添加节点
						Button {
							text: qsTr("添加节点")
							width: parent.width - 20
							onClicked: {
								var id = nodeFlow.addNode("新节点", "副标题", "#2E7DD1", Qt.point(100, 100))
								console.log("添加节点成功，ID:", id)
							}
						}

						// 删除选中节点
						Button {
							text: qsTr("删除选中节点")
							width: parent.width - 20
							onClicked: {
								if (nodeFlow.selectedNodeId >= 0) {
									nodeFlow.removeSelectedNode()
									console.log("节点已删除")
								} else {
									console.log("请先选中节点")
								}
							}
						}

						// 清空图
						Button {
							text: qsTr("清空图")
							width: parent.width - 20
							onClicked: {
								nodeFlow.clearGraph()
								console.log("图已清空")
							}
						}

						Text {
							text: qsTr("连线操作")
							font.bold: true
							color: jaspTheme.textEnabled
						}

						// 添加连线
						Button {
							text: qsTr("添加连线 (1→2)")
							width: parent.width - 20
							onClicked: {
								// 先确保有至少两个节点
								if (nodeFlow.nodeCount < 2) {
									nodeFlow.addNode("节点1", "", "#2E7DD1", Qt.point(50, 50))
									nodeFlow.addNode("节点2", "", "#1F9D63", Qt.point(200, 50))
								}
								nodeFlow.addEdge(1, 2, "next")
								console.log("连线已添加")
							}
						}

						// 删除选中连线
						Button {
							text: qsTr("删除选中连线")
							width: parent.width - 20
							onClicked: {
								if (nodeFlow.selectedEdgeIndex >= 0) {
									nodeFlow.removeSelectedEdge()
									console.log("连线已删除")
								} else {
									console.log("请先选中连线")
								}
							}
						}

						Text {
							text: qsTr("视图操作")
							font.bold: true
							color: jaspTheme.textEnabled
						}

						Row {
							spacing: 5
							Button {
								text: qsTr("放大")
								onClicked: nodeFlow.zoomIn()
							}
							Button {
								text: qsTr("缩小")
								onClicked: nodeFlow.zoomOut()
							}
							Button {
								text: qsTr("重置")
								onClicked: nodeFlow.resetView()
							}
						}

						Button {
							text: qsTr("适应视图")
							width: parent.width - 20
							onClicked: nodeFlow.fitToView()
						}

						Text {
							text: qsTr("数据操作")
							font.bold: true
							color: jaspTheme.textEnabled
						}

						Row {
							spacing: 5
							Button {
								text: qsTr("导出 JSON")
								onClicked: {
									var success = nodeFlow.exportJson("/tmp/nodeflow_export.json")
									console.log("导出结果:", success)
								}
							}
							Button {
								text: qsTr("导入 JSON")
								onClicked: {
									var success = nodeFlow.importJson("/tmp/nodeflow_export.json")
									console.log("导入结果:", success)
								}
							}
						}

						Text {
							text: qsTr("状态控制")
							font.bold: true
							color: jaspTheme.textEnabled
						}

						Row {
							spacing: 5
							CheckBox {
								text: qsTr("显示网格")
								checked: nodeFlow.gridVisible
								onCheckedChanged: nodeFlow.gridVisible = checked
							}
							CheckBox {
								text: qsTr("运行动画")
								checked: nodeFlow.running
								onCheckedChanged: nodeFlow.running = checked
							}
						}

						CheckBox {
							text: qsTr("连接模式")
							checked: nodeFlow.connectionMode
							onCheckedChanged: nodeFlow.connectionMode = checked
						}

						// 状态显示
						Rectangle {
							width: parent.width - 20
							height: 100
							color: jaspTheme.white
							border.color: jaspTheme.borderColor
							border.width: 1

							ScrollView {
								anchors.fill: parent
								TextArea {
									id: statusArea
									readOnly: true
									text: "节点数: " + nodeFlow.nodeCount +
										  "\n连线数: " + nodeFlow.edgeCount +
										  "\n缩放: " + nodeFlow.zoom.toFixed(2) +
										  "\n选中节点ID: " + nodeFlow.selectedNodeId +
										  "\n选中连线索引: " + nodeFlow.selectedEdgeIndex
									color: jaspTheme.textEnabled
								}
							}
						}

						// 关闭按钮
						Button {
							text: qsTr("关闭调试面板")
							width: parent.width - 20
							onClicked: nodeFlowDebugPanel.visible = false
						}
					}
				}

				// 右侧 NodeFlow 视图
				Rectangle {
					width: parent.width - 290
					height: parent.height
					color: jaspTheme.white
					border.color: jaspTheme.borderColor
					border.width: 1

					JC.NodeFlow {
						id: nodeFlow
						anchors.fill: parent
						focus: true  // 确保能接收键盘事件

						// 连接信号以便调试
						Connections {
							target: nodeFlow

							function onGraphChanged(nodeCount, edgeCount) {
								console.log("图已改变 - 节点:", nodeCount, "连线:", edgeCount)
								statusArea.text = "节点数: " + nodeCount +
												  "\n连线数: " + edgeCount +
												  "\n缩放: " + nodeFlow.zoom.toFixed(2) +
												  "\n选中节点ID: " + nodeFlow.selectedNodeId +
												  "\n选中连线索引: " + nodeFlow.selectedEdgeIndex
							}

							function onNodeSelected(id, title) {
								console.log("节点选中 - ID:", id, "标题:", title)
							}

							function onEdgeSelected(from, to, label) {
								console.log("连线选中 - 从:", from, "到:", to, "标签:", label)
							}

							function onZoomChanged(zoom) {
								console.log("缩放改变:", zoom)
							}

							function onContextMenuRequested(itemX, itemY, nodeId, edgeIndex, sceneX, sceneY) {
								console.log("右键菜单请求 - 本地坐标:", itemX, itemY,
											"场景坐标:", sceneX, sceneY,
											"节点ID:", nodeId, "连线索引:", edgeIndex)

								// 这里可以弹出自定义菜单
								// 示例：使用 Qt Quick Controls 的 Menu
								var menu = Qt.createQmlObject('
									import QtQuick.Controls
									Menu {
										MenuItem {
											text: "添加节点"
											onTriggered: nodeFlow.addNode("新节点", "", "#2E7DD1", Qt.point(' + sceneX + ', ' + sceneY + '))
										}
										MenuItem {
											text: "删除"
											enabled: ' + (nodeId >= 0 || edgeIndex >= 0) + '
											onTriggered: nodeFlow.removeSelectedItem()
										}
									}', nodeFlow)
								menu.popup(itemX, itemY)
							}

							function onNodeTitleEditRequested(id, currentTitle) {
								console.log("请求编辑节点标题 - ID:", id, "当前标题:", currentTitle)
								// 这里可以弹出输入对话框
								// 示例：使用 Qt Quick Dialogs
								var dialog = Qt.createQmlObject('
									import QtQuick.Controls
									Dialog {
										title: "编辑节点标题"
										standardButtons: Dialog.Ok | Dialog.Cancel
										TextField {
											id: titleField
											text: "' + currentTitle + '"
											width: parent.width
										}
										onAccepted: nodeFlow.setNodeTitle(' + id + ', titleField.text)
									}', nodeFlow)
								dialog.open()
							}

							function onEdgeLabelEditRequested(index, currentLabel) {
								console.log("请求编辑连线标签 - 索引:", index, "当前标签:", currentLabel)
								// 类似于节点标题编辑
							}
						}

						// 初始化一些测试数据
						Component.onCompleted: {
							// 添加几个测试节点
							addNode("开始", "流程起点", "#2E7DD1", Qt.point(50, 50))
							addNode("处理", "数据处理", "#1F9D63", Qt.point(200, 50))
							addNode("输出", "结果输出", "#D87516", Qt.point(350, 50))

							// 添加测试连线
							addEdge(1, 2, "next")
							addEdge(2, 3, "next")

							// 适应视图
							fitToView()

							console.log("NodeFlow 初始化完成")
						}
					}
				}
			}
		}

		// ============================================================
		// 在 RibbonBar 中添加切换按钮（示例）
		// ============================================================
		// 在您的 RibbonBar 组件中添加一个按钮来切换 NodeFlow 调试面板的显示
		// 例如，在 RibbonBar 的某个工具栏中添加：

		Button {
			id: nodeFlowDebugToggle
			text: qsTr("NodeFlow 调试")
			checkable: true
			checked: nodeFlowDebugPanel.visible
			onCheckedChanged: nodeFlowDebugPanel.visible = checked
			z: 200   // 必须高于 ribbon 的 z:6，否则还是会被压住

			anchors
			{
				top:		parent.top
				right:		parent.right
				topMargin:	4
				rightMargin: 4
			}
		}


		MouseArea
		{
			//visible:					enabled
			enabled: 					fileMenuModel.visible || modulesMenu.opened || customMenu.visible
			z:							enabled ? 1 : -5
			hoverEnabled:				true
			onContainsMouseChanged:		if(containsMouse) ribbonModel.highlightedModuleIndex = -1
			anchors.fill:				parent
			propagateComposedEvents:	true

			Rectangle
			{
				id:				darkeningBackgroundRect;
				color:			jaspTheme.darkeningColour
				anchors.fill:	parent;
				opacity:		visible ? 0.4 : 0.0
				visible:		fileMenuModel.visible || modulesMenu.opened

				Behavior on opacity
				{
					enabled:		preferencesModel.animationsOn

					PropertyAnimation
					{
						id:				darkeningBackgroundRectDarkening
						duration:		jaspTheme.fileMenuSlideDuration
						easing.type:	Easing.OutCubic
					}
				}
			}

			onPressed: (mouse)=>
			{
				if(customMenu.visible)
				{
					customMenu.hideMenus()
					mouse.accepted = false;
				}

				fileMenuModel.visible	= false
				modulesMenu.opened		= false
				ribbon.focusOutRibbonBar();
			}
		}

		ModulesMenu
		{
			id:			modulesMenu
			z:			2

			anchors
			{
				top:	ribbon.bottom
				right:	parent.right
				bottom:	parent.bottom
			}
		}

		CreateComputeColumnDialog	{ id: createComputeDialog	}
		ModuleInstaller				{ id: moduleInstallerDialog	}
		ResizeDataDialog			{ id: resizeDataDialog		}
		RenameColumnDialog			{ id: renameColumnDialog	}
		PlotEditor					{ id: plotEditingDialog		}

		/*MessageBox
		{
			id:	msgBox
			z:	2

			Connections
			{
				target:			mainWindow
				onShowWarning:	msgBox.showWarning(title, message)
			}
		}*/
	}

	UIScaleNotifier { anchors.centerIn:	parent }

	ProgressBarHolder
	{
		visible:			mainWindow.progressBarVisible && !csvPreviewModel.visible
		z:					10
		anchors.fill:		parent
	}
	
	Rectangle
	{
		z:				11
		visible:		csvPreviewModel.visible
		color:			"#000000"
		opacity:		0.25
		anchors.fill:	parent
	}


	Image
	{
		id:					chatToggleButton
		z:					99
		visible:			preferencesModel.aiEnabled
		width:				45 * preferencesModel.uiScale
		height:				45 * preferencesModel.uiScale
		opacity:			mainWindow.aiChatVisible && mainWindow.chatWindowActive ? 1.0 : 0.55
		source:				personaAvatar ? personaAvatar : jaspTheme.iconPath + "jaspAI.png"
		sourceSize.width:	width
		sourceSize.height:	height
		fillMode:			Image.PreserveAspectFit

		ToolTip.visible:	chatMouseArea.containsMouse
		ToolTip.text:		qsTr("Toggle AI Chat (Ctrl+J)")
		ToolTip.delay:		500

		anchors
		{
			right:		parent.right
			bottom:		parent.bottom
			rightMargin:	jaspTheme.scrollbarBoxWidthBig + 3 * preferencesModel.uiScale
			bottomMargin:	mainWindow.welcomePageVisible ? 85 * preferencesModel.uiScale : jaspTheme.scrollbarBoxWidthBig + 3 * preferencesModel.uiScale
		}

		MouseArea
		{
			id:				chatMouseArea
			anchors.fill:	parent
			cursorShape:	Qt.PointingHandCursor
			hoverEnabled:	true
			onClicked:		mainWindow.toggleChat()
		}
	}

}
