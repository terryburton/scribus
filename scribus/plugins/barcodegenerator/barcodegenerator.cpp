/*
For general Scribus (>=1.3.2) copyright and licensing information please refer
to the COPYING file provided with the program. Following this notice may exist
a copyright and/or license notice that predates the release of Scribus 1.3.2
for which a new license (GPL+exception) is in place.
*/

#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

#include "../formatidlist.h"
#include "barcodegenerator.h"
#include "commonstrings.h"
#include "iconmanager.h"
#include "loadsaveplugin.h"
#include "scpaths.h"
#include "ui/helpbrowser.h"
#include "scribus.h"
#include "scribuscore.h"
#include "scribusview.h"
#include "selection.h"
#include "ui/colorsandfills.h"
#include "undomanager.h"

static constexpr int debounceInterval = 250;  // ms

BarcodeType::BarcodeType(const QString &cmd, const QString &exa, const QString &exaop)
	: command(cmd),
	  exampleContents(exa),
	  exampleOptions(exaop)
{

}

BarcodeGenerator::BarcodeGenerator(QWidget* parent, const char* name)
	: QDialog(parent)
{
	ui.setupUi(this);
	setObjectName(name);
	setModal(true);

	ui.bcodeBox->layout()->setAlignment(Qt::AlignTop);
	ui.colorBox->layout()->setAlignment(Qt::AlignTop);

	connect(&thread, SIGNAL(renderedImage(QString)),this, SLOT(updatePreview(QString)));

	QString barcodeFile = ScPaths::instance().shareDir() + QString("/plugins/barcode.ps");
	try
	{
		m_bwipp.emplace(bwipp::InitOpts{}.filename(barcodeFile.toLocal8Bit().constData()).lazy_load(true));
	}
	catch (const std::exception &)
	{
		qDebug() << "Barcodegenerator unable to load" << barcodeFile;
		return;
	}

	struct BarcodeMetadata {
		QString desc;
		QString exam;
		QString exop;
	};
	QHash<QString, BarcodeMetadata> metadata;
	QList<QString> encoderlist;
	for (const auto &encoder : m_bwipp->list_encoders())
	{
		QString enc = QString::fromLatin1(encoder.c_str());
		encoderlist.append(enc);
		BarcodeMetadata& md = metadata[enc];
		std::string v;
		if (!(v = m_bwipp->get_property(encoder, "DESC")).empty())
			md.desc = QString::fromUtf8(v.c_str());
		if (!(v = m_bwipp->get_property(encoder, "EXAM")).empty())
			md.exam = QString::fromUtf8(v.c_str());
		if (!(v = m_bwipp->get_property(encoder, "EXOP")).empty())
			md.exop = QString::fromUtf8(v.c_str());
	}

	// Load UI configuration (combos, checkboxes, family ordering) from JSON
	loadUIConfig(ScPaths::instance().shareDir() + QString("/plugins/barcode_ui.json"));

	// Apply desc/exam/exop overrides from JSON before building the map
	for (auto it = encoderUI.constBegin(); it != encoderUI.constEnd(); ++it)
	{
		const QString& enc = it.key();
		const BarcodeEncoderUI& eui = it.value();
		BarcodeMetadata& md = metadata[enc];
		if (!eui.desc.isEmpty())
			md.desc = eui.desc;
		if (!eui.exam.isEmpty())
			md.exam = eui.exam;
		if (!eui.exop.isEmpty())
			md.exop = eui.exop;
	}

	foreach (const QString& enc, encoderlist)
	{
		if (encoderUI.contains(enc) && !encoderUI.value(enc).enabled)
			continue;
		const BarcodeMetadata& md = metadata[enc];
		map[md.desc] = BarcodeType(enc, md.exam, md.exop);
	}

	// Building up the bcFamilyCombo grouping the formats for readability
	ui.bcFamilyCombo->addItem(tr("Select a barcode family")); // to prevent 1st gs call
	ui.bcFamilyCombo->insertSeparator(999);

	// Building up the bcCombo grouping the formats for readability
	ui.bcCombo->addItem(tr("Select a barcode format")); // to prevent 1st gs call
	ui.bcCombo->insertSeparator(999);

	for (const auto &fam : m_bwipp->list_families())
	{
		QString familyName = QString::fromUtf8(fam.c_str());
		const BarcodeFamilyUI& fui = familyUI[familyName];
		if (!fui.enabled)
			continue;
		familyList.append(familyName);
		QStringList bcNames;
		for (const auto &member : m_bwipp->list_family_members(fam))
		{
			QString enc = QString::fromLatin1(member.c_str());
			if (encoderUI.contains(enc) && !encoderUI.value(enc).enabled)
				continue;
			if (metadata.contains(enc))
				bcNames.append(metadata[enc].desc);
		}
		familyItems.insert(familyName, bcNames);
	}

	// Sort families by order then name
	std::sort(familyList.begin(), familyList.end(), [this](const QString& a, const QString& b) {
		int oa = familyUI.value(a).order;
		int ob = familyUI.value(b).order;
		if (oa != ob) return oa < ob;
		return a.compare(b, Qt::CaseInsensitive) < 0;
	});

	// Build reverse lookup: display name -> encoder command
	QHash<QString, QString> descToEnc;
	for (auto m = map.cbegin(); m != map.cend(); ++m)
		descToEnc[m.key()] = m.value().command;

	// Sort encoders within each family by order then description
	for (auto it = familyItems.begin(); it != familyItems.end(); ++it)
	{
		QStringList& names = it.value();
		std::sort(names.begin(), names.end(), [this, &descToEnc](const QString& a, const QString& b) {
			int oa = encoderUI.value(descToEnc.value(a)).order;
			int ob = encoderUI.value(descToEnc.value(b)).order;
			if (oa != ob) return oa < ob;
			return a.compare(b, Qt::CaseInsensitive) < 0;
		});
	}

	ui.bcFamilyCombo->addItems(familyList);

	ui.okButton->setText(CommonStrings::tr_OK);
	ui.cancelButton->setText(CommonStrings::tr_Cancel);
	ui.resetButton->setIcon(IconManager::instance().loadIcon("u_undo"));

	if (ScCore->primaryMainWindow()->doc->PageColors.contains("Black"))
	{
		lnColor = ScCore->primaryMainWindow()->doc->PageColors["Black"];
		txtColor = ScCore->primaryMainWindow()->doc->PageColors["Black"];
		ui.linesLabel->setToolTip("Black");
		ui.txtLabel->setToolTip("Black");
	}
	else
	{
		ui.linesLabel->setToolTip("n.a.");
		ui.txtLabel->setToolTip("n.a.");
	}
	if (ScCore->primaryMainWindow()->doc->PageColors.contains("White"))
	{
		bgColor = ScCore->primaryMainWindow()->doc->PageColors["White"];
		ui.bgLabel->setToolTip("White");
	}
	else
		ui.bgLabel->setToolTip("n.a.");

	paintColorSample(ui.linesLabel, lnColor);
	paintColorSample(ui.txtLabel, txtColor);
	paintColorSample(ui.bgLabel, bgColor);

	paintBarcodeTimer = new QTimer(this);
	paintBarcodeTimer->setSingleShot(true);
	connect(paintBarcodeTimer, SIGNAL(timeout()), this, SLOT(paintBarcode()));

	syncOptionsUITimer = new QTimer(this);
	syncOptionsUITimer->setSingleShot(true);
	connect(syncOptionsUITimer, SIGNAL(timeout()), this, SLOT(syncOptionsUI()));

	connect(ui.bcFamilyCombo, SIGNAL(activated(int)), this, SLOT(bcFamilyComboChanged()));
	connect(ui.bcCombo, SIGNAL(activated(int)), this, SLOT(bcComboChanged()));
	connect(ui.bgColorButton, SIGNAL(clicked()), this, SLOT(bgColorButton_pressed()));
	connect(ui.lnColorButton, SIGNAL(clicked()), this, SLOT(lnColorButton_pressed()));
	connect(ui.txtColorButton, SIGNAL(clicked()), this, SLOT(txtColorButton_pressed()));
	ui.helpSymbologiesButton->setIcon(IconManager::instance().loadIcon("help-browser"));
	ui.helpOptionsButton->setIcon(IconManager::instance().loadIcon("help-browser"));
	connect(ui.helpSymbologiesButton, SIGNAL(clicked()), this, SLOT(helpSymbologiesButton_pressed()));
	connect(ui.helpOptionsButton, SIGNAL(clicked()), this, SLOT(helpOptionsButton_pressed()));
	connect(ui.okButton, SIGNAL(clicked()), this, SLOT(okButton_pressed()));
	connect(ui.cancelButton, SIGNAL(clicked()), this, SLOT(cancelButton_pressed()));
	connect(ui.codeEdit, SIGNAL(textChanged(QString)), this, SLOT(codeEdit_textChanged(QString)));
	connect(ui.resetButton, SIGNAL(clicked()), this, SLOT(resetButton_clicked()));
	bcComboChanged();

}

BarcodeGenerator::~BarcodeGenerator()
{
	QFile::remove(QDir::toNativeSeparators(ScPaths::tempFileDir() + "bcode.ps"));
	QFile::remove(QDir::toNativeSeparators(ScPaths::tempFileDir() + "bcode.png"));
	if (m_helpBrowser)
	{
		m_helpBrowser->close();
		delete m_helpBrowser;
		m_helpBrowser = nullptr;
	}
	if (!paintBarcodeTimer)
		return;
	delete paintBarcodeTimer;
	paintBarcodeTimer = nullptr;
}

static BarcodeComboConfig parseComboConfig(const QJsonObject& obj)
{
	BarcodeComboConfig cfg;
	cfg.name = obj.value("name").toString();
	cfg.key = obj.value("key").toString();
	const QJsonArray arr = obj.value("values").toArray();
	for (const QJsonValue& v : arr)
		cfg.values.append(v.toString());
	return cfg;
}

void BarcodeGenerator::loadUIConfig(const QString& path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
	{
		qDebug() << "Barcodegenerator: barcode_ui.json not found at" << path;
		return;
	}

	QJsonParseError err;
	QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
	f.close();
	if (doc.isNull())
	{
		qDebug() << "Barcodegenerator: barcode_ui.json parse error:" << err.errorString();
		return;
	}

	QJsonObject root = doc.object();

	// Load family configuration
	QJsonObject famObj = root.value("families").toObject();
	for (auto it = famObj.begin(); it != famObj.end(); ++it)
	{
		BarcodeFamilyUI fui;
		QJsonObject fo = it.value().toObject();
		if (fo.contains("enabled"))
			fui.enabled = fo.value("enabled").toBool();
		if (fo.contains("order"))
			fui.order = fo.value("order").toInt();
		if (fo.contains("desc"))
			fui.desc = fo.value("desc").toString();
		familyUI[it.key()] = fui;
	}

	// Load encoder configuration
	QJsonObject encObj = root.value("encoders").toObject();
	for (auto it = encObj.begin(); it != encObj.end(); ++it)
	{
		BarcodeEncoderUI eui;
		QJsonObject eo = it.value().toObject();
		if (eo.contains("enabled"))
			eui.enabled = eo.value("enabled").toBool();
		if (eo.contains("order"))
			eui.order = eo.value("order").toInt();
		if (eo.contains("desc"))
			eui.desc = eo.value("desc").toString();
		if (eo.contains("exam"))
			eui.exam = eo.value("exam").toString();
		if (eo.contains("exop"))
			eui.exop = eo.value("exop").toString();
		if (eo.contains("combo1"))
			eui.combo1 = parseComboConfig(eo.value("combo1").toObject());
		if (eo.contains("combo2"))
			eui.combo2 = parseComboConfig(eo.value("combo2").toObject());
		eui.includetext = eo.value("includetext").toBool();
		eui.guardwhitespace = eo.value("guardwhitespace").toBool();
		eui.includecheck = eo.value("includecheck").toBool();
		eui.includecheckintext = eo.value("includecheckintext").toBool();
		eui.parse = eo.value("parse").toBool();
		eui.parsefnc = eo.value("parsefnc").toBool();
		encoderUI[it.key()] = eui;
	}
}

void BarcodeGenerator::loadBarcode(const QString& encoder, const QString& content, const QString& options)
{
	// Reverse-lookup encoder command to find the display name
	QString displayName;
	QString familyName;
	for (auto it = map.cbegin(); it != map.cend(); ++it)
	{
		if (it.value().command == encoder)
		{
			displayName = it.key();
			break;
		}
	}

	if (displayName.isEmpty())
		return;

	// Find the family containing this display name
	for (auto it = familyItems.cbegin(); it != familyItems.cend(); ++it)
	{
		if (it.value().contains(displayName))
		{
			familyName = it.key();
			break;
		}
	}

	// Set family combo
	if (!familyName.isEmpty())
	{
		int familyIdx = ui.bcFamilyCombo->findText(familyName);
		if (familyIdx >= 0)
		{
			ui.bcFamilyCombo->setCurrentIndex(familyIdx);
			bcFamilyComboChanged();
		}
	}

	// Set barcode format combo
	int bcIdx = ui.bcCombo->findText(displayName);
	if (bcIdx >= 0)
	{
		ui.bcCombo->setCurrentIndex(bcIdx);
		// Don't call bcComboChanged() yet - we'll set content/options first
		updateOptions();
	}

	// Set content and options
	ui.codeEdit->blockSignals(true);
	ui.codeEdit->setText(content);
	ui.codeEdit->blockSignals(false);

	ui.optionsEdit->blockSignals(true);
	ui.optionsEdit->setText(options);
	ui.optionsEdit->blockSignals(false);

	updateUIFromOptionsText();

	// Enable controls
	ui.codeEdit->setEnabled(true);
	ui.optionsEdit->setEnabled(true);
	ui.bgColorButton->setEnabled(true);
	ui.lnColorButton->setEnabled(true);
	ui.txtColorButton->setEnabled(true);
	ui.okButton->setEnabled(true);
}

void BarcodeGenerator::loadFromItem(PageItem* item)
{
	m_editItem = item;

	QString encoder = item->getObjectAttribute("bwipp-encoder").value;
	QString content = item->getObjectAttribute("bwipp-content").value;
	QString options = item->getObjectAttribute("bwipp-options").value;

	loadBarcode(encoder, content, options);
	enqueuePaintBarcode(0);
}

void BarcodeGenerator::loadFromParams(const QMap<QString, QString>& params)
{
	loadBarcode(params.value("bwipp-encoder"),
				params.value("bwipp-content"),
				params.value("bwipp-options"));
}

static bool parseBwippColor(const QString& hex, ScColor& color)
{
	bool ok;
	if (hex.length() == 6)
	{
		// RRGGBB
		int r = hex.mid(0, 2).toInt(&ok, 16); if (!ok) return false;
		int g = hex.mid(2, 2).toInt(&ok, 16); if (!ok) return false;
		int b = hex.mid(4, 2).toInt(&ok, 16); if (!ok) return false;
		color.setRgbColor(r, g, b);
		return true;
	}
	if (hex.length() == 8)
	{
		// CCMMYYKK
		int c = hex.mid(0, 2).toInt(&ok, 16); if (!ok) return false;
		int m = hex.mid(2, 2).toInt(&ok, 16); if (!ok) return false;
		int y = hex.mid(4, 2).toInt(&ok, 16); if (!ok) return false;
		int k = hex.mid(6, 2).toInt(&ok, 16); if (!ok) return false;
		color.setCmykColor(c, m, y, k);
		return true;
	}
	return false;
}

// Find the value for a key=value token in a space-separated options string.
// Returns the value if found, or a null QString if not present.
static QString optGetValue(const QStringList& tokens, const QString& key)
{
	const QString prefix = key + "=";
	for (const QString& t : tokens)
		if (t.startsWith(prefix))
			return t.mid(prefix.length());
	return QString();
}

// Check whether a bare keyword token is present
static bool optHasKeyword(const QStringList& tokens, const QString& key)
{
	return tokens.contains(key);
}

// Set key=value in a token list, replacing any existing key= token
static void optSetValue(QStringList& tokens, const QString& key, const QString& value)
{
	const QString prefix = key + "=";
	for (int i = 0; i < tokens.size(); ++i)
	{
		if (tokens[i].startsWith(prefix))
		{
			tokens[i] = key + "=" + value;
			return;
		}
	}
	tokens.append(key + "=" + value);
}

// Remove all tokens matching key= or bare keyword
static void optRemoveKey(QStringList& tokens, const QString& key)
{
	const QString prefix = key + "=";
	for (int i = tokens.size() - 1; i >= 0; --i)
		if (tokens[i] == key || tokens[i].startsWith(prefix))
			tokens.removeAt(i);
}

void BarcodeGenerator::updateOptionValue(const QString& key, const QString& value)
{
	QStringList tokens = ui.optionsEdit->text().split(' ', Qt::SkipEmptyParts);
	optSetValue(tokens, key, value);
	ui.optionsEdit->blockSignals(true);
	ui.optionsEdit->setText(tokens.join(' '));
	ui.optionsEdit->blockSignals(false);
}

void BarcodeGenerator::ensureOptionPresent(const QString& key)
{
	QStringList tokens = ui.optionsEdit->text().split(' ', Qt::SkipEmptyParts);
	if (!optHasKeyword(tokens, key))
	{
		tokens.append(key);
		ui.optionsEdit->blockSignals(true);
		ui.optionsEdit->setText(tokens.join(' '));
		ui.optionsEdit->blockSignals(false);
	}
}

void BarcodeGenerator::updateOptions()
{
	QString enc = map[ui.bcCombo->currentText()].command;
	const BarcodeEncoderUI& eui = encoderUI[enc];

	ui.formatLabel->setText(eui.combo1.name.isEmpty() ? "Version:" : eui.combo1.name + ":");
	ui.formatCombo->blockSignals(true);
	ui.formatCombo->clear();
	ui.formatCombo->addItem("Auto");
	if (!eui.combo1.values.isEmpty())
	{
		ui.formatCombo->insertSeparator(999);
		ui.formatCombo->addItems(eui.combo1.values);
		ui.formatLabel->setEnabled(true);
		ui.formatCombo->setEnabled(true);
	}
	else
	{
		ui.formatLabel->setEnabled(false);
		ui.formatCombo->setEnabled(false);
	}
	ui.formatCombo->blockSignals(false);

	ui.eccLabel->setText(eui.combo2.name.isEmpty() ? "EC Level:" : eui.combo2.name + ":");
	ui.eccCombo->blockSignals(true);
	ui.eccCombo->clear();
	ui.eccCombo->addItem("Auto");
	if (!eui.combo2.values.isEmpty())
	{
		ui.eccCombo->insertSeparator(999);
		ui.eccCombo->addItems(eui.combo2.values);
		ui.eccLabel->setEnabled(true);
		ui.eccCombo->setEnabled(true);
	}
	else
	{
		ui.eccLabel->setEnabled(false);
		ui.eccCombo->setEnabled(false);
	}
	ui.eccCombo->blockSignals(false);

}


void BarcodeGenerator::bcFamilyComboChanged()
{
	ui.bcCombo->blockSignals(true);
	ui.bcCombo->clear();
	ui.bcCombo->addItem(tr("Select a barcode format")); // to prevent 1st gs call
	ui.bcCombo->insertSeparator(999);
	ui.bcCombo->addItems(familyItems[ui.bcFamilyCombo->currentText()]);
	ui.bcCombo->blockSignals(false);
	bcComboChanged();
}

void BarcodeGenerator::bcComboChanged(int)
{
	bcComboChanged();
}

void BarcodeGenerator::bcComboChanged()
{
	updateOptions();

	if (ui.bcCombo->currentIndex() == 0)
	{
		ui.okButton->setEnabled(false);
		ui.sampleLabel->setText(tr("Select Type"));
		ui.codeEdit->clear();
		ui.codeEdit->setEnabled(false);
		ui.optionsEdit->clear();
		ui.optionsEdit->setEnabled(false);
		ui.includetextCheck->setEnabled(false);
		ui.guardwhitespaceCheck->setEnabled(false);
		ui.includecheckCheck->setEnabled(false);
		ui.includecheckintextCheck->setEnabled(false);
		ui.parseCheck->setEnabled(false);
		ui.parsefncCheck->setEnabled(false);
		ui.formatLabel->setEnabled(false);
		ui.formatCombo->setEnabled(false);
		ui.eccLabel->setEnabled(false);
		ui.eccCombo->setEnabled(false);
		ui.bgColorButton->setEnabled(false);
		ui.lnColorButton->setEnabled(false);
		ui.txtColorButton->setEnabled(false);
		return;
	}

	ui.codeEdit->setEnabled(true);
	ui.optionsEdit->setEnabled(true);
	ui.bgColorButton->setEnabled(true);
	ui.lnColorButton->setEnabled(true);
	ui.txtColorButton->setEnabled(true);
	ui.okButton->setEnabled(true);

	QString s = ui.bcCombo->currentText();
	ui.codeEdit->blockSignals(true);
	ui.codeEdit->setText(map[s].exampleContents);
	ui.codeEdit->blockSignals(false);
	ui.optionsEdit->blockSignals(true);
	ui.optionsEdit->setText(map[s].exampleOptions);
	ui.optionsEdit->blockSignals(false);

	QString enc = map[s].command;
	const BarcodeEncoderUI& eui = encoderUI[enc];
	ui.includetextCheck->setEnabled(eui.includetext);
	ui.guardwhitespaceCheck->setEnabled(eui.guardwhitespace);
	ui.includecheckCheck->setEnabled(eui.includecheck);
	ui.includecheckintextCheck->setEnabled(eui.includetext && eui.includecheckintext);
	ui.parseCheck->setEnabled(eui.parse);
	ui.parsefncCheck->setEnabled(eui.parsefnc);

	updateUIFromOptionsText();

	enqueuePaintBarcode(0);
}

void BarcodeGenerator::enqueuePaintBarcode(int delay)
{
	ui.okButton->setEnabled(false);
	paintBarcodeTimer->start(delay);
}

void BarcodeGenerator::updateOptionsTextFromUI()
{
	QStringList tokens = ui.optionsEdit->text().split(' ', Qt::SkipEmptyParts);

	const std::initializer_list<std::pair<QCheckBox*, const char*>> boolOpts = {
		{ui.includetextCheck, "includetext"},
		{ui.guardwhitespaceCheck, "guardwhitespace"},
		{ui.includecheckCheck, "includecheck"},
		{ui.includecheckintextCheck, "includecheckintext"},
		{ui.parseCheck, "parse"},
		{ui.parsefncCheck, "parsefnc"},
	};
	for (const auto& [cb, kw] : boolOpts)
	{
		QString key = QString::fromLatin1(kw);
		if (cb->isChecked())
		{
			if (!optHasKeyword(tokens, key))
				tokens.append(key);
		}
		else
			tokens.removeAll(key);
	}

	QString enc = map[ui.bcCombo->currentText()].command;
	const BarcodeEncoderUI& eui = encoderUI[enc];
	QString combo1Key = eui.combo1.key.isEmpty() ? "version" : eui.combo1.key;
	QString combo2Key = eui.combo2.key.isEmpty() ? "eclevel" : eui.combo2.key;

	if (ui.formatCombo->currentIndex() != 0)
		optSetValue(tokens, combo1Key, ui.formatCombo->currentText());
	else
		optRemoveKey(tokens, combo1Key);

	if (ui.eccCombo->currentIndex() != 0)
		optSetValue(tokens, combo2Key, ui.eccCombo->currentText());
	else
		optRemoveKey(tokens, combo2Key);

	if (ui.inkspreadSlider->value() > 0)
		optSetValue(tokens, "inkspread", QString::number(ui.inkspreadSlider->value() / 100.0, 'f', 2));
	else
		optRemoveKey(tokens, "inkspread");

	ui.optionsEdit->blockSignals(true);
	ui.optionsEdit->setText(tokens.join(' '));
	ui.optionsEdit->blockSignals(false);
}

void BarcodeGenerator::updateUIFromOptionsText()
{
	QStringList tokens = ui.optionsEdit->text().split(' ', Qt::SkipEmptyParts);

	auto setCheckIfChanged = [](QCheckBox* cb, bool val) {
		if (cb->isChecked() != val)
		{
			cb->blockSignals(true);
			cb->setChecked(val);
			cb->blockSignals(false);
		}
	};

	setCheckIfChanged(ui.includetextCheck, optHasKeyword(tokens, "includetext"));
	setCheckIfChanged(ui.guardwhitespaceCheck, optHasKeyword(tokens, "guardwhitespace"));
	setCheckIfChanged(ui.includecheckCheck, optHasKeyword(tokens, "includecheck"));
	setCheckIfChanged(ui.includecheckintextCheck, optHasKeyword(tokens, "includecheckintext"));
	setCheckIfChanged(ui.parseCheck, optHasKeyword(tokens, "parse"));
	setCheckIfChanged(ui.parsefncCheck, optHasKeyword(tokens, "parsefnc"));

	QString enc = map[ui.bcCombo->currentText()].command;
	const BarcodeEncoderUI& eui = encoderUI[enc];
	QString combo1Key = eui.combo1.key.isEmpty() ? "version" : eui.combo1.key;
	QString combo2Key = eui.combo2.key.isEmpty() ? "eclevel" : eui.combo2.key;

	QString fmtVal = optGetValue(tokens, combo1Key);
	int fmtIdx = fmtVal.isNull() ? 0 : ui.formatCombo->findText(fmtVal);
	if (fmtIdx == -1)
		fmtIdx = 0;
	if (ui.formatCombo->currentIndex() != fmtIdx)
	{
		ui.formatCombo->blockSignals(true);
		ui.formatCombo->setCurrentIndex(fmtIdx);
		ui.formatCombo->blockSignals(false);
	}

	QString eccVal = optGetValue(tokens, combo2Key);
	int eccIdx = eccVal.isNull() ? 0 : ui.eccCombo->findText(eccVal);
	if (eccIdx == -1)
		eccIdx = 0;
	if (ui.eccCombo->currentIndex() != eccIdx)
	{
		ui.eccCombo->blockSignals(true);
		ui.eccCombo->setCurrentIndex(eccIdx);
		ui.eccCombo->blockSignals(false);
	}

	// Sync inkspread slider from options text
	QString inkVal = optGetValue(tokens, "inkspread");
	int inkInt = inkVal.isNull() ? 0 : qBound(0, (int)(inkVal.toDouble() * 100), 25);
	if (ui.inkspreadSlider->value() != inkInt)
	{
		ui.inkspreadSlider->blockSignals(true);
		ui.inkspreadSlider->setValue(inkInt);
		ui.inkspreadSlider->blockSignals(false);
	}
	ui.inkspreadValue->setText(QString::number(inkInt / 100.0, 'f', 2));

	// Sync color members from options text
	ScColor parsed;

	QString lnVal = optGetValue(tokens, "barcolor");
	if (!lnVal.isNull() && parseBwippColor(lnVal, parsed) && !(parsed == lnColor))
	{
		lnColor = parsed;
		ui.linesLabel->setToolTip(lnVal);
		paintColorSample(ui.linesLabel, lnColor);
	}

	QString bgVal = optGetValue(tokens, "backgroundcolor");
	if (!bgVal.isNull() && parseBwippColor(bgVal, parsed) && !(parsed == bgColor))
	{
		bgColor = parsed;
		ui.bgLabel->setToolTip(bgVal);
		paintColorSample(ui.bgLabel, bgColor);
	}

	QString txtVal = optGetValue(tokens, "textcolor");
	if (!txtVal.isNull() && parseBwippColor(txtVal, parsed) && !(parsed == txtColor))
	{
		txtColor = parsed;
		ui.txtLabel->setToolTip(txtVal);
		paintColorSample(ui.txtLabel, txtColor);
	}
}

void BarcodeGenerator::updatePreview(const QString& errorMsg)
{
	QString pngFile = QDir::toNativeSeparators(ScPaths::tempFileDir() + "bcode.png");
	if (errorMsg.isEmpty())
	{
		ui.sampleLabel->setPixmap(QPixmap(pngFile));
		ui.okButton->setEnabled(true);
	}
	else
	{
		ui.sampleLabel->setText("<qt>" + errorMsg + "</qt>");
	}
}

void BarcodeGenerator::on_includetextCheck_stateChanged(int)
{
	updateOptionsTextFromUI();
	enqueuePaintBarcode(0);
}

void BarcodeGenerator::on_guardwhitespaceCheck_stateChanged(int)
{
	updateOptionsTextFromUI();
	enqueuePaintBarcode(0);
}

void BarcodeGenerator::on_includecheckCheck_stateChanged(int)
{
	updateOptionsTextFromUI();
	enqueuePaintBarcode(0);
}

void BarcodeGenerator::on_includecheckintextCheck_stateChanged(int)
{
	updateOptionsTextFromUI();
	enqueuePaintBarcode(0);
}

void BarcodeGenerator::on_parseCheck_stateChanged(int)
{
	updateOptionsTextFromUI();
	enqueuePaintBarcode(0);
}

void BarcodeGenerator::on_parsefncCheck_stateChanged(int)
{
	updateOptionsTextFromUI();
	enqueuePaintBarcode(0);
}

void BarcodeGenerator::on_formatCombo_currentIndexChanged(int)
{
	updateOptionsTextFromUI();
	enqueuePaintBarcode(0);
}

void BarcodeGenerator::on_eccCombo_currentIndexChanged(int)
{
	updateOptionsTextFromUI();
	enqueuePaintBarcode(0);
}

void BarcodeGenerator::on_inkspreadSlider_valueChanged(int value)
{
	ui.inkspreadValue->setText(QString::number(value / 100.0, 'f', 2));
	updateOptionsTextFromUI();
	enqueuePaintBarcode(debounceInterval);
}

void BarcodeGenerator::paintColorSample(QLabel *l, const ScColor & c)
{
	QPixmap currentPixmap = l->pixmap(Qt::ReturnByValue);
	QSize pixmapSize(currentPixmap.width(), currentPixmap.height());
	if (currentPixmap.isNull())
	{
		QRect rect = l->frameRect();
		double pixelRatio = l->devicePixelRatioF();
		pixmapSize = QSize(rect.width() * pixelRatio, rect.height() * pixelRatio);
	}
	QPixmap pm(pixmapSize.width(), pixmapSize.height());
	pm.fill(c.getRawRGBColor()); // brute force sc2qt color convert for preview
	l->setPixmap(pm);
}

void BarcodeGenerator::bgColorButton_pressed()
{
	ColorsAndFillsDialog d(this, &ScCore->primaryMainWindow()->doc->docGradients, ScCore->primaryMainWindow()->doc->PageColors, "", &ScCore->primaryMainWindow()->doc->docPatterns, ScCore->primaryMainWindow()->doc, ScCore->primaryMainWindow());
	if (!d.exec())
		return;

	QString selectedColorName = d.selectedColorName();
	if (selectedColorName == CommonStrings::None)
		return;

	bgColor = d.selectedColor();
	ui.bgLabel->setToolTip(d.selectedColorName());
	paintColorSample(ui.bgLabel, bgColor);
	QString hex = bgColor.name().replace('#', "").toUpper();
	updateOptionValue("backgroundcolor", hex);
	ensureOptionPresent("showbackground");
	enqueuePaintBarcode(0);
}

void BarcodeGenerator::lnColorButton_pressed()
{
	ColorsAndFillsDialog d(this, &ScCore->primaryMainWindow()->doc->docGradients, ScCore->primaryMainWindow()->doc->PageColors, "", &ScCore->primaryMainWindow()->doc->docPatterns, ScCore->primaryMainWindow()->doc, ScCore->primaryMainWindow());
	if (!d.exec())
		return;

	QString selectedColorName = d.selectedColorName();
	if (selectedColorName == CommonStrings::None)
		return;

	lnColor = d.selectedColor();
	ui.linesLabel->setToolTip(d.selectedColorName());
	paintColorSample(ui.linesLabel, lnColor);
	QString hex = lnColor.name().replace('#', "").toUpper();
	updateOptionValue("barcolor", hex);
	enqueuePaintBarcode(0);
}

void BarcodeGenerator::txtColorButton_pressed()
{
	ColorsAndFillsDialog d(this, &ScCore->primaryMainWindow()->doc->docGradients, ScCore->primaryMainWindow()->doc->PageColors, "", &ScCore->primaryMainWindow()->doc->docPatterns, ScCore->primaryMainWindow()->doc, ScCore->primaryMainWindow());
	if (!d.exec())
		return;

	QString selectedColorName = d.selectedColorName();
	if (selectedColorName == CommonStrings::None)
		return;

	txtColor = d.selectedColor();
	ui.txtLabel->setToolTip(d.selectedColorName());
	paintColorSample(ui.txtLabel, txtColor);
	QString hex = txtColor.name().replace('#', "").toUpper();
	updateOptionValue("textcolor", hex);
	enqueuePaintBarcode(0);
}

bool BarcodeGenerator::generateBarcode(PageItem* replaceItem, double placeX, double placeY)
{
	QString psFile = QDir::toNativeSeparators(ScPaths::tempFileDir() + "bcode.ps");

	// Write PS file synchronously (the preview path writes it via the
	// render thread, but callers like the silent-regeneration path may
	// not have run a preview)
	{
		QFile f(psFile);
		if (!f.open(QIODevice::WriteOnly))
			return false;
		QTextStream ts(&f);
		ts << buildPSCommand();
	}

	const FileFormat* fmt = LoadSavePlugin::getFormatByExt("ps");
	if (!fmt)
		return false;

	ScribusMainWindow* mw = ScCore->primaryMainWindow();
	ScribusDoc* doc = mw->doc;

	UndoTransaction tran;
	if (UndoManager::undoEnabled())
	{
		tran = UndoManager::instance()->beginTransaction(
					doc->currentPage()->getUName(),
					Um::IImageFrame,
					Um::ImportBarcode,
					ui.bcCombo->currentText() + " (" + ui.codeEdit->text() + ")",
					Um::IEPS);
	}

	// Save geometry of item being replaced
	double ox = 0, oy = 0, orot = 0, scaleX = 1.0, scaleY = 1.0;
	bool oar = false;
	if (replaceItem)
	{
		ox = replaceItem->xPos();
		oy = replaceItem->yPos();
		orot = replaceItem->rotation();
		oar = replaceItem->aspectRatioLocked();
		double storedNativeW = replaceItem->getObjectAttribute("bwipp-nativeWidth").value.toDouble();
		double storedNativeH = replaceItem->getObjectAttribute("bwipp-nativeHeight").value.toDouble();
		scaleX = (storedNativeW > 0) ? replaceItem->width() / storedNativeW : 1.0;
		scaleY = (storedNativeH > 0) ? replaceItem->height() / storedNativeH : 1.0;
	}

	int itemsBefore = doc->Items->count();
	fmt->loadFile(psFile, LoadSavePlugin::lfUseCurrentPage
				  | LoadSavePlugin::lfInteractive
				  | LoadSavePlugin::lfScripted
				  | LoadSavePlugin::lfNoDialogs
				  | LoadSavePlugin::lfLockAspectRatio);

	PageItem* newItem = nullptr;
	double nativeW = 0, nativeH = 0;
	if (doc->Items->count() > itemsBefore)
	{
		newItem = doc->Items->last();
		nativeW = newItem->width();
		nativeH = newItem->height();
	}

	if (newItem && replaceItem)
	{
		// Apply relative scaling and restore geometry
		newItem->setXYPos(ox, oy);
		newItem->setWidthHeight(nativeW * scaleX, nativeH * scaleY);
		newItem->SetRectFrame();
		newItem->ClipEdited = true;
		newItem->setRotation(orot);
		newItem->setAspectRatioLocked(oar);

		// Delete original, select replacement
		doc->m_Selection->clear();
		doc->m_Selection->addItem(replaceItem);
		doc->itemSelection_DeleteItem();
		doc->m_Selection->clear();
		doc->m_Selection->addItem(newItem);
	}
	else if (newItem && placeX >= 0 && placeY >= 0)
	{
		newItem->setXYPos(placeX, placeY);
	}
	else if (newItem)
	{
		// New barcode: center on current page
		ScPage* page = doc->currentPage();
		newItem->setXYPos(
					page->xOffset() + (page->width() - nativeW) / 2.0,
					page->yOffset() + (page->height() - nativeH) / 2.0);
		doc->m_Selection->setGroupRect();
		if (doc->view())
			doc->view()->DrawNew();
	}

	// Attach barcode attributes directly on the new item
	if (newItem)
	{
		ObjAttrVector attrs;
		auto addAttr = [&attrs](const QString& name, const QString& value) {
			ObjectAttribute attr;
			attr.name = name;
			attr.type = "string";
			attr.value = value;
			attrs.append(attr);
		};
		addAttr("bwipp-encoder", map[ui.bcCombo->currentText()].command);
		addAttr("bwipp-content", ui.codeEdit->text());
		addAttr("bwipp-options", ui.optionsEdit->text());
		addAttr("bwipp-nativeWidth", QString::number(nativeW, 'f', 6));
		addAttr("bwipp-nativeHeight", QString::number(nativeH, 'f', 6));
		addAttr("plugin-editAction", "BarcodeGenerator");
		newItem->setObjectAttributes(&attrs);
	}

	if (tran)
		tran.commit();

	return true;
}

void BarcodeGenerator::showHelpBrowser(const QString& file)
{
	if (!m_helpBrowser)
	{
		m_helpBrowser = new HelpBrowser(this, tr("Barcode Reference"), "en", "", file);
		m_helpBrowser->setWindowFlags(m_helpBrowser->windowFlags() | Qt::Tool);
		connect(m_helpBrowser, &HelpBrowser::closed, this, [this]() {
			m_helpBrowser->deleteLater();
			m_helpBrowser = nullptr;
		});
	}
	else
	{
		m_helpBrowser->jumpToHelpSection("", file, false);
	}
	m_helpBrowser->show();
	m_helpBrowser->raise();
	m_helpBrowser->activateWindow();
}

void BarcodeGenerator::helpSymbologiesButton_pressed()
{
	showHelpBrowser("bwipp-symbologies.html");
}

void BarcodeGenerator::helpOptionsButton_pressed()
{
	showHelpBrowser("bwipp-options.html");
}

void BarcodeGenerator::okButton_pressed()
{
	hide();
	generateBarcode(m_editItem);
	m_editItem = nullptr;
	accept();
}

void BarcodeGenerator::cancelButton_pressed()
{
	reject();
}

void BarcodeGenerator::codeEdit_textChanged(const QString&)
{
	enqueuePaintBarcode(0);
}

void BarcodeGenerator::on_optionsEdit_textChanged(const QString&)
{
	syncOptionsUITimer->start(debounceInterval);
}

void BarcodeGenerator::syncOptionsUI()
{
	updateUIFromOptionsText();
	enqueuePaintBarcode(0);
}

QString BarcodeGenerator::buildPSCommand()
{
	QString opts = ui.optionsEdit->text();

	// Only append default colors for values NOT already in the options string
	QStringList tokens = opts.split(' ', Qt::SkipEmptyParts);
	if (optGetValue(tokens, "barcolor").isNull())
		opts += " barcolor=" + lnColor.name().replace('#', "").toUpper();
	if (optGetValue(tokens, "backgroundcolor").isNull())
		opts += " showbackground backgroundcolor=" + bgColor.name().replace('#', "").toUpper();
	if (optGetValue(tokens, "textcolor").isNull())
		opts += " textcolor=" + txtColor.name().replace('#', "").toUpper();

	// Assemble PS from encoder and requirement bodies
	QString psCommand = "%!PS-Adobe-2.0 EPSF-2.0\n"
					"currentglobal true setglobal\n"
					"/uk.co.terryburton.bwipp.global_ctx << /default_inkspread 0 >> def\n"
					"setglobal\n";
	QString enc = map[ui.bcCombo->currentText()].command;
	std::string resources = m_bwipp->emit_required_resources(enc.toLatin1().constData());
	if (!resources.empty())
		psCommand.append(QString::fromLatin1(resources.c_str()));
	psCommand.append(
				"errordict begin\n"
				"/handleerror {\n"
				"$error begin\n"
				"errorname dup length string cvs 0 6 getinterval (bwipp.) eq {\n"
				"(%stderr) (w) file\n"
				"dup (\nBWIPP ERROR: ) writestring\n"
				"dup errorname dup length string cvs writestring\n"
				"dup ( ) writestring\n"
				"dup errorinfo dup length string cvs writestring\n"
				"dup (\n) writestring\n"
				"dup flushfile end quit\n"
				"} if\n"
				"end //handleerror exec\n"
				"} bind def\n"
				"end\n"
				);
	QString comm("20 10 moveto <%1> <%2> /%3 /uk.co.terryburton.bwipp findresource exec\n");
	QString bcString = ui.codeEdit->text();
	QByteArray bcLatin1 = ui.codeEdit->text().toLatin1();
	QByteArray bcUtf8 = ui.codeEdit->text().toUtf8();
	QByteArray bcArray = (bcString != QString::fromLatin1(bcLatin1)) ? ("\xef\xbb\xbf" + bcUtf8) : bcLatin1;
	QString bcdata(bcArray.toHex());
	QString bcopts(opts.toLatin1().toHex());
	comm = comm.arg(bcdata, bcopts, map[ui.bcCombo->currentText()].command);
	psCommand.append(comm);
	psCommand.append("showpage\n");

	return psCommand;
}

void BarcodeGenerator::paintBarcode()
{
	thread.render(buildPSCommand());
}


void BarcodeGenerator::resetButton_clicked()
{
	bcComboChanged();
}
