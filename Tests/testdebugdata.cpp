#include "testinfo.h"
#include "tempfiles.h"
#include "processinfo.h"
#include "testdebugdata.h"
#include "utilities/qutils.h"
#include "databaseinterface.h"
#include "utilities/settings.h"
#include "data/datasetpackage.h"
#include "data/importers/csvimporter.h"

void TestDebugData::initTestCase()
{
	TempFiles::init(ProcessInfo::currentPID()); // needed here so that the LRNAM can be passed the session directory

}

void TestDebugData::init()
{
	TempFiles::clearSessionDir();
	Settings::informSettingsThatThisIsATest();
	
	_pkg		= new DataSetPackage(this);
	_importer	= new CSVImporter();
	
	_importer->loadDataSet(fq(_testLibrary().absoluteFilePath("csv/debug.csv")), [](int i){});

	_data = _pkg->dataSet();
	
	Column * facFive = _data->column("facFive");
	
	if(!facFive->hasLabels())
		facFive->noLabelsToLabels();
}

void TestDebugData::cleanup()
{
	if(_data)
		delete _data;
	_data = nullptr;

	DatabaseInterface::singleton()->close();
	DatabaseInterface::singleton()->closeInterfaces();
	
	if(_pkg)
		delete _pkg;
	_pkg = nullptr;
	
	if(_importer)
		delete _importer;
	_importer = nullptr;
}


void TestDebugData::testReverseNumericals()
{
	QVERIFY2(_data,						"No dataset!");
	
	Column * facFive = _data->column("facFive");
	
	QVERIFY2(facFive,						"No facFive!");
	
	Json::Value labelsBefore =	facFive->serializeLabels(true);
								facFive->valuesReverse();
	Json::Value labelsAfter1 =	facFive->serializeLabels(true);
								facFive->valuesReverse();
	Json::Value labelsAfter2 =	facFive->serializeLabels(true);
	
	QVERIFY2(labelsBefore == labelsAfter2,		"Reversing values is not reversible!");
	QVERIFY2(labelsBefore != labelsAfter1,		"Reversing values does not change the labels!");
	
	const std::string jsonReversed = R"Something(
[
	{
		"description" : "",
		"filterAllows" : true,
		"label" : "5",
		"order" : 0,
		"originalValue" : "1"
	},
	{
		"description" : "",
		"filterAllows" : true,
		"label" : "4",
		"order" : 1,
		"originalValue" : "2"
	},
	{
		"description" : "",
		"filterAllows" : true,
		"label" : "",
		"order" : 2,
		"originalValue" : "3"
	},
	{
		"description" : "",
		"filterAllows" : true,
		"label" : "2",
		"order" : 3,
		"originalValue" : "4"
	},
	{
		"description" : "",
		"filterAllows" : true,
		"label" : "1",
		"order" : 4,
		"originalValue" : "5"
	}
]
)Something";
	
	Json::Value hardcoded;
	Json::Reader parser;
	
	parser.parse(jsonReversed, hardcoded);
	
	if(hardcoded != labelsAfter1)
		std::cerr << labelsAfter1 << std::endl;
	
	QVERIFY2(hardcoded == labelsAfter1,		"Reversing values is not right!");
	
	
	DataSet loadMe(_data->id());
	QVERIFY2(_data->jsonForCompare() == loadMe.jsonForCompare(), "DataSet isnt the same after dbload!");
	
}


void TestDebugData::testReverseLabels()
{
	QVERIFY2(_data,		"No dataset!");
	
	Column * facFive = _data->column("facFive");
	
	QVERIFY2(facFive,	"No facFive!");
	
	facFive->setAutoSortByValue(false);
	
	Json::Value labelsBefore =	facFive->serializeLabels(true);
								facFive->labelsReverse();
	Json::Value labelsAfter1 =	facFive->serializeLabels(true);
								facFive->labelsReverse();
	Json::Value labelsAfter2 =	facFive->serializeLabels(true);
	
	QVERIFY2(labelsBefore == labelsAfter2,		"Reversing labels is not reversible!");
	QVERIFY2(labelsBefore != labelsAfter1,		"Reversing labels does not change the labels!");
	
	const std::string jsonReversed = R"Something(
[
	{
		"description" : "",
		"filterAllows" : true,
		"label" : "",
		"order" : 0,
		"originalValue" : "5"
	},
	{
		"description" : "",
		"filterAllows" : true,
		"label" : "",
		"order" : 1,
		"originalValue" : "4"
	},
	{
		"description" : "",
		"filterAllows" : true,
		"label" : "",
		"order" : 2,
		"originalValue" : "3"
	},
	{
		"description" : "",
		"filterAllows" : true,
		"label" : "",
		"order" : 3,
		"originalValue" : "2"
	},
	{
		"description" : "",
		"filterAllows" : true,
		"label" : "",
		"order" : 4,
		"originalValue" : "1"
	}
]
)Something";
	
	Json::Value hardcoded;
	Json::Reader parser;
	
	parser.parse(jsonReversed, hardcoded);
	
	QVERIFY2(hardcoded == labelsAfter1,		"Reversing values is not right!");
	
	if(hardcoded != labelsAfter1)
		std::cerr << labelsAfter1 << std::endl;
	
	DataSet loadMe(_data->id());
	QVERIFY2(_data->jsonForCompare() == loadMe.jsonForCompare(), "DataSet isnt the same after dbload!");
}

void TestDebugData::testColumnStuff()
{
	QVERIFY2(_data,		"No dataset!");
	
	auto originalColumnCount = _data->columnCount();
	auto originalColumnNames = _data->getColumnNames();
	
	stringset	removeUs = { "debInf", "contcor1", "contcor2", "debString" },
				oriNames = stringset(originalColumnNames.begin(), originalColumnNames.end());
	
	for(auto & remMe : removeUs)
	{
		QVERIFY2(oriNames.count(remMe),		"Test column to remove doesnt exist");
		QVERIFY2(_data->column(remMe),		"Looking up column failed");
		
		_data->removeColumn(remMe);
		QVERIFY2(_data->columnCount() == originalColumnCount - 1,	"Columncount didnt decrease");
		
		originalColumnCount = _data->columnCount();
		
		QVERIFY2(!_data->column(remMe),		"Column is still there!");
	}
	
	Column * V1 = _data->column("V1");
	QVERIFY2(V1, "Column V1 is missing...");
	
	V1->setName("Variable 1");
	QVERIFY2(V1->name()  == "Variable 1", "Rename failed");
	QVERIFY2(V1->title() == "Variable 1", "Rename failed to also change the title");
	
	Column * Var1 = _data->column("Variable 1");
	QVERIFY2(Var1, "Rename column didnt update the dataset lookup");
	QVERIFY2(Var1 == V1, "Renamed column is not the same column");
	
	V1->setTitle("Something else entirely");
	QVERIFY2(V1->title() == "Something else entirely", "Retitle failed");
	
	V1->setName("Var1");
	QVERIFY2(V1->name()  == "Var1", "Rename failed");
	QVERIFY2(V1->title() == "Something else entirely", "Got retitled even though it shouldnt have been!");
	
	V1->setTitle("Var1");
	QVERIFY2(V1->title() == "Var1", "Retitle failed");
	
	V1->setName("Variable 1");
	QVERIFY2(V1->name()  == "Variable 1", "Rename failed");
	QVERIFY2(V1->title() == "Variable 1", "Rename failed to also change the title");

	
	DataSet loadMe(_data->id());
	QVERIFY2(_data->jsonForCompare() == loadMe.jsonForCompare(), "DataSet isnt the same after dbload!");
	
	
	
}

void TestDebugData::testEmptyValues()
{
	QVERIFY2(_data,		"No dataset!");
	
	Column * contBinom = _data->column("contBinom");
	
	if(!contBinom->hasLabels())
		contBinom->noLabelsToLabels();
	
	QVERIFY2(contBinom->nonEmptyLevelsStrings().size() == 2, "Not right amount of non-empty labels!");
	
	contBinom->setHasCustomEmptyValues(true);
	contBinom->setCustomEmptyValues({"1"});
	QVERIFY2(contBinom->nonEmptyLevelsStrings().size() == 1,	"Not right amount of non-empty labels after adding one!");
	QVERIFY2(contBinom->nonEmptyLevelsStrings()[0] == "0",		"Not right non-empty label left after adding one empty value!");
	
	contBinom->setCustomEmptyValues({"0"});
	QVERIFY2(contBinom->nonEmptyLevelsStrings().size() == 1,	"Not right amount of non-empty labels after changing one empty value into another!");
	QVERIFY2(contBinom->nonEmptyLevelsStrings()[0] == "1",		"Not right non-empty label left after adding one empty value!");

	
	contBinom->setCustomEmptyValues({"0", "1"});
	QVERIFY2(contBinom->nonEmptyLevelsStrings().size() == 0,	"There should be no labels anymore!");
	
	contBinom->setHasCustomEmptyValues(false);
	QVERIFY2(contBinom->nonEmptyLevelsStrings().size() == 2,	"Not right amount of non-empty labels after disabling custom empty values!");
	
	_data->setWorkspaceEmptyValues({"1"});
	QVERIFY2(contBinom->nonEmptyLevelsStrings().size() == 1,	"Not right amount of non-empty labels after adding one empty value to workspace!");
	QVERIFY2(contBinom->nonEmptyLevelsStrings()[0] == "0",		"Not right non-empty label left after adding one empty value to workspace!");
	
	contBinom->setHasCustomEmptyValues(true);
	contBinom->setCustomEmptyValues({"0"});
	QVERIFY2(contBinom->nonEmptyLevelsStrings().size() == 0,	"There should be no labels anymore!");
}

void TestDebugData::testChangeLabel()
{
	QVERIFY2(_data,		"No dataset!");
	
	Column * contBinom = _data->column("contBinom");
	
	QVERIFY2(contBinom->hasLabels(),							"contBinom should have labels");
	
	DataSetPackage::pkg()->setData(DataSetPackage::pkg()->indexForSubNode(contBinom->labels()[0]), "A", int(DataSetPackage::specialRoles::label));
	
	QVERIFY2(contBinom->labels()[0]->labelDisplay() == "A",		"contBinom failed renaming first label to A");
	
	DataSetPackage::pkg()->setData(DataSetPackage::pkg()->indexForSubNode(contBinom->labels()[1]), "B", int(DataSetPackage::specialRoles::value));
	
	QVERIFY2(contBinom->labels()[1]->labelDisplay() == "B",		"contBinom failed renaming first value (and thus also label!) to B");
	
}

void TestDebugData::testShadowDisplay()
{
	QVERIFY2(_data,		"No dataset!");
	
	Column * contNormal = _data->column("contNormal");
	
	QVERIFY2(contNormal->type() == columnType::scale,			"contNormal should be scale type");
	
	contNormal->labelsToNoLabels();
	
	QVERIFY2(!contNormal->hasLabels(),							"contNormal should not have labels");
	
	for (size_t row = 0; row < 10; row++)
	{
		std::string value = contNormal->getValue(row, false, true);
		std::string display = contNormal->getDisplay(row, false, false);
		std::string shadow = contNormal->getShadow(row, false, false);
		
	QString shadMsg = QString("Row %1: Shadow should not be empty when display would be empty! (value='%2', display='%3', shadow='%4')")
				.arg(row).arg(QString::fromStdString(value)).arg(QString::fromStdString(display)).arg(QString::fromStdString(shadow));
		std::string shadMsgStr = shadMsg.toStdString();
		QVERIFY2(!shadow.empty() || value.empty(), shadMsgStr.c_str());
	}
	
	Column * contWide = _data->column("contWide");
	
	QVERIFY2(contWide->type() == columnType::scale,			"contWide should be scale type");
	
	contWide->labelsToNoLabels();
	
	QVERIFY2(!contWide->hasLabels(),							"contWide should not have labels");
	
	std::string val5 = contWide->getValue(5, false, true);
	std::string disp5 = contWide->getDisplay(5, false, false);
	std::string shad5 = contWide->getShadow(5, false, false);
	
	QString shad5Msg = QString("contWide row 5 shadow should not be empty (val='%1', disp='%2', shad='%3')")
				.arg(val5.c_str()).arg(disp5.c_str()).arg(shad5.c_str());
		std::string shad5MsgStr = shad5Msg.toStdString();
		QVERIFY2(!shad5.empty(), shad5MsgStr.c_str());
	
	Column * contBinom = _data->column("contBinom");
	
	if(!contBinom->hasLabels())
		contBinom->noLabelsToLabels();
	
	QVERIFY2(contBinom->hasLabels(),							"contBinom should have labels now");
	
	std::string valBinom0 = contBinom->getValue(0, false, true);
	std::string dispBinom0 = contBinom->getDisplay(0, false, false);
	std::string shadBinom0 = contBinom->getShadow(0, false, false);
	
	QString shadBinom0Msg = QString("contBinom row 0 shadow should not be empty (val='%1', disp='%2', shad='%3')")
				.arg(valBinom0.c_str()).arg(dispBinom0.c_str()).arg(shadBinom0.c_str());
		std::string shadBinom0MsgStr = shadBinom0Msg.toStdString();
		QVERIFY2(!shadBinom0.empty(), shadBinom0MsgStr.c_str());
	
	Column * contGamma = _data->column("contGamma");
	
	QVERIFY2(contGamma->type() == columnType::scale,			"contGamma should be scale type");
	
	contGamma->labelsToNoLabels();
	
	QVERIFY2(!contGamma->hasLabels(),							"contGamma should not have labels");
	
	std::string valContGamma = contGamma->getValue(0, false, true);
	std::string dispContGamma = contGamma->getDisplay(0, false, false);
	std::string shadContGamma = contGamma->getShadow(0, false, false);
	
QString shadContGammaMsg = QString("contGamma row 0 shadow should not be empty (val='%1', disp='%2', shad='%3')")
				.arg(valContGamma.c_str()).arg(dispContGamma.c_str()).arg(shadContGamma.c_str());
		std::string shadContGammaMsgStr = shadContGammaMsg.toStdString();
		QVERIFY2(!shadContGamma.empty(), shadContGammaMsgStr.c_str());
	}


QTEST_MAIN(TestDebugData)
