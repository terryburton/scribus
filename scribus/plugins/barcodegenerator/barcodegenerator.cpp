/*
For general Scribus (>=1.3.2) copyright and licensing information please refer
to the COPYING file provided with the program. Following this notice may exist
a copyright and/or license notice that predates the release of Scribus 1.3.2
for which a new license (GPL+exception) is in place.
*/

#include <QButtonGroup>
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
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

	// Equal stretch for widget columns in text grid (labels=0, widgets=1)
	for (int c = 0; c < 8; ++c)
		ui.textGridLayout->setColumnStretch(c, c % 2);

	// Text block tab button group
	m_textTabGroup = new QButtonGroup(this);
	m_textTabGroup->setExclusive(true);
	for (int i = 1; i <= 9; ++i)
		m_textTabGroup->addButton(findChild<QPushButton*>(QString("textTab%1").arg(i)), i);
	connect(m_textTabGroup, &QButtonGroup::idClicked, this, [this](int id) {
		updateOptionsTextFromUI();
		m_activeTextTab = id;
		updateUIFromOptionsText();
		updateTextControlsEnabled();
		enqueuePaintBarcode(0);
	});

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

	syncOptionsTextTimer = new QTimer(this);
	syncOptionsTextTimer->setSingleShot(true);
	connect(syncOptionsTextTimer, &QTimer::timeout, this, [this]() {
		updateOptionsTextFromUI();
		updateTextControlsEnabled();
		enqueuePaintBarcode(0);
	});

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

	// UI controls → options text sync (immediate render)
	auto immediateSync = [this]() { updateOptionsTextFromUI(); enqueuePaintBarcode(0); };
	for (auto* cb : {ui.includetextCheck, ui.guardwhitespaceCheck, ui.includecheckCheck,
					  ui.includecheckintextCheck, ui.parseCheck, ui.parsefncCheck,
					  ui.dottyCheck, ui.cropCheck})
		connect(cb, &QCheckBox::stateChanged, this, immediateSync);
	for (auto* combo : {ui.formatCombo, ui.eccCombo, ui.textfontCombo,
						 ui.textdirectionCombo, ui.textxalignCombo, ui.textyalignCombo})
		connect(combo, &QComboBox::currentIndexChanged, this, immediateSync);
	for (auto* radio : {ui.borderBorderRadio, ui.borderBearerRadio, ui.borderNoneRadio})
		connect(radio, &QRadioButton::toggled, this, [immediateSync](bool checked) { if (checked) immediateSync(); });

	// UI controls → options text sync (debounced render)
	auto debouncedSync = [this]() { updateOptionsTextFromUI(); enqueuePaintBarcode(debounceInterval); };
	for (auto* combo : {ui.textsizeCombo, ui.textgapsCombo, ui.textxoffsetCombo,
						 ui.textyoffsetCombo, ui.alttextsubspaceCombo, ui.alttextsplitCombo})
		connect(combo, &QComboBox::currentTextChanged, this, debouncedSync);
	for (auto* spin : {ui.borderwidthSpin, ui.borderleftSpin, ui.borderrightSpin,
					    ui.bordertopSpin, ui.borderbottomSpin})
		connect(spin, &QDoubleSpinBox::valueChanged, this, debouncedSync);

	// Sliders with label updates
	connect(ui.heightSlider, &QSlider::valueChanged, this, [this](int value) {
		if (value > 0 && value < 20)
		{
			ui.heightSlider->blockSignals(true);
			ui.heightSlider->setValue(20);
			ui.heightSlider->blockSignals(false);
			value = 20;
		}
		ui.heightValue->setText(value == 0 ? tr("Auto") : QString::number(value / 100.0, 'f', 2));
		updateOptionsTextFromUI();
		enqueuePaintBarcode(debounceInterval);
	});
	connect(ui.inkspreadSlider, &QSlider::valueChanged, this, [this](int value) {
		ui.inkspreadValue->setText(QString::number(value / 100.0, 'f', 2));
		updateOptionsTextFromUI();
		enqueuePaintBarcode(debounceInterval);
	});

	// Options text field → UI sync (debounced)
	connect(ui.optionsEdit, &QPlainTextEdit::textChanged, this, [this]() {
		syncOptionsUITimer->start(debounceInterval);
	});

	// Alt text → options text (debounced)
	connect(ui.alttextEdit, &QPlainTextEdit::textChanged, this, [this]() {
		syncOptionsTextTimer->start(debounceInterval);
	});

	// Populate text size combo: 4, 5, 6, ... 20
	for (int i = 4; i <= 20; ++i)
		ui.textsizeCombo->addItem(QString::number(i));

	// Populate text gaps combo: 0.0, 0.5, 1.0, ... 20.0
	for (int i = 0; i <= 40; ++i)
		ui.textgapsCombo->addItem(QString::number(i / 2.0, 'f', 1));

	ui.alttextEdit->installEventFilter(this);
	ui.alttextEdit->viewport()->installEventFilter(this);
	ui.alttextEdit->document()->setDocumentMargin(2);
	ui.optionsEdit->installEventFilter(this);
	ui.optionsEdit->viewport()->installEventFilter(this);
	int collapsedHeight = ui.codeEdit->sizeHint().height();
	ui.optionsEdit->document()->setDocumentMargin(2);
	ui.optionsEdit->setMinimumHeight(collapsedHeight);
	ui.optionsEdit->setMaximumHeight(collapsedHeight);
	ui.alttextEdit->setMinimumHeight(collapsedHeight);
	ui.alttextEdit->setMaximumHeight(collapsedHeight);
	ui.alttextsubspaceCombo->lineEdit()->setMaxLength(1);
	ui.alttextsubspaceCombo->lineEdit()->installEventFilter(this);
	ui.alttextsplitCombo->lineEdit()->setMaxLength(1);
	ui.alttextsplitCombo->lineEdit()->installEventFilter(this);

	// Populate text offset combos: -10.00, -9.00, ... 10.00
	for (auto* combo : {ui.textxoffsetCombo, ui.textyoffsetCombo})
		for (int i = -10; i <= 10; ++i)
			combo->addItem(QString::number(i, 'f', 2));

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
		eui.dotty = eo.value("dotty").toBool();
		eui.dottyForced = eo.value("dottyForced").toBool();
		eui.height = eo.value("height").toBool();
		eui.bearer = eo.value("bearer").toBool();
		eui.fixedtext = eo.value("fixedtext").toBool();
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
	ui.optionsEdit->setPlainText(options);
	ui.optionsEdit->blockSignals(false);

	updateUIFromOptionsText();
	setControlsEnabled(true);
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
	QStringList tokens = ui.optionsEdit->toPlainText().split(' ', Qt::SkipEmptyParts);
	optSetValue(tokens, key, value);
	QString newOpts = tokens.join(' ');
	if (ui.optionsEdit->toPlainText() != newOpts)
	{
		ui.optionsEdit->blockSignals(true);
		ui.optionsEdit->setPlainText(newOpts);
		ui.optionsEdit->blockSignals(false);
	}
}

void BarcodeGenerator::ensureOptionPresent(const QString& key)
{
	QStringList tokens = ui.optionsEdit->toPlainText().split(' ', Qt::SkipEmptyParts);
	if (!optHasKeyword(tokens, key))
	{
		tokens.append(key);
		ui.optionsEdit->blockSignals(true);
		ui.optionsEdit->setPlainText(tokens.join(' '));
		ui.optionsEdit->blockSignals(false);
	}
}

// Map text option suffix to option key for the active text block tab.
// Tab 1: "textfont", "textsize", etc.
// Tab 2: "extratextfont", "extratextsize", etc.
// Tab 3-9: "text3font", "text3size", etc.
QString BarcodeGenerator::textOptKey(const QString& suffix) const
{
	if (m_activeTextTab == 1) return "text" + suffix;
	if (m_activeTextTab == 2) return "extratext" + suffix;
	return "text" + QString::number(m_activeTextTab) + suffix;
}

// Map alttext subkey to option key for the active text block tab.
// Tab 1: "alttext", "alttextsubspace", "alttextsplit"
// Tab 2: "extratext", "extratextsubspace", "extratextsplit"
// Tab 3-9: "text3", "text3subspace", "text3split"
QString BarcodeGenerator::altTextKey(const QString& subkey) const
{
	if (m_activeTextTab == 1) return subkey.isEmpty() ? "alttext" : ("alttext" + subkey);
	if (m_activeTextTab == 2) return "extratext" + subkey;
	return "text" + QString::number(m_activeTextTab) + subkey;
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

	// Per-encoder checkboxes
	ui.guardwhitespaceCheck->setEnabled(eui.guardwhitespace);
	ui.includecheckCheck->setEnabled(eui.includecheck);
	ui.includecheckintextCheck->setEnabled(eui.includetext && eui.includecheckintext);
	ui.parseCheck->setEnabled(eui.parse);
	ui.parsefncCheck->setEnabled(eui.parsefnc);
	if (eui.dottyForced)
	{
		ui.dottyCheck->blockSignals(true);
		ui.dottyCheck->setChecked(true);
		ui.dottyCheck->setEnabled(false);
		ui.dottyCheck->blockSignals(false);
	}
	else
	{
		ui.dottyCheck->setEnabled(eui.dotty);
	}

	// Height slider
	ui.heightLabel->setEnabled(eui.height);
	ui.heightSlider->setEnabled(eui.height);
	ui.heightValue->setEnabled(eui.height);

	// Bearer bars
	ui.borderBearerRadio->setEnabled(eui.bearer);
	if (!eui.bearer && ui.borderBearerRadio->isChecked())
	{
		ui.borderNoneRadio->blockSignals(true);
		ui.borderNoneRadio->setChecked(true);
		ui.borderNoneRadio->blockSignals(false);
	}

	updateTextControlsEnabled();
}

void BarcodeGenerator::updateTextControlsEnabled()
{
	QString enc = map[ui.bcCombo->currentText()].command;
	const BarcodeEncoderUI& eui = encoderUI[enc];

	// For fixedtext encoders on tab 1, text positioning controls are
	// disabled unless alttext overrides the encoder's native text
	bool fixed = eui.fixedtext && m_activeTextTab == 1
		&& ui.alttextEdit->toPlainText().isEmpty();

	ui.textdirectionCombo->setEnabled(!fixed);
	ui.textgapsCombo->setEnabled(!fixed);
	ui.textxalignCombo->setEnabled(!fixed);
	ui.textyalignCombo->setEnabled(!fixed);
	ui.textxoffsetCombo->setEnabled(!fixed);
	ui.textyoffsetCombo->setEnabled(!fixed);
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
	m_activeTextTab = 1;
	if (auto* btn = m_textTabGroup->button(1))
		btn->setChecked(true);

	updateOptions();

	if (ui.bcCombo->currentIndex() == 0)
	{
		setControlsEnabled(false);
		ui.sampleLabel->setText(tr("Select Type"));
		ui.codeEdit->clear();
		ui.optionsEdit->clear();
		return;
	}

	setControlsEnabled(true);

	QString s = ui.bcCombo->currentText();
	ui.codeEdit->blockSignals(true);
	ui.codeEdit->setText(map[s].exampleContents);
	ui.codeEdit->blockSignals(false);
	ui.optionsEdit->blockSignals(true);
	ui.optionsEdit->setPlainText(map[s].exampleOptions);
	ui.optionsEdit->blockSignals(false);

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
	QStringList tokens = ui.optionsEdit->toPlainText().split(' ', Qt::SkipEmptyParts);

	const std::initializer_list<std::pair<QCheckBox*, const char*>> boolOpts = {
		{ui.includetextCheck, "includetext"},
		{ui.guardwhitespaceCheck, "guardwhitespace"},
		{ui.includecheckCheck, "includecheck"},
		{ui.includecheckintextCheck, "includecheckintext"},
		{ui.parseCheck, "parse"},
		{ui.parsefncCheck, "parsefnc"},
		{ui.dottyCheck, "dotty"},
		{ui.cropCheck, "crop"},
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

	// Height slider
	int hVal = ui.heightSlider->value();
	if (hVal >= 20)
		optSetValue(tokens, "height", QString::number(hVal / 100.0, 'f', 2));
	else
		optRemoveKey(tokens, "height");

	// Text formatting options
	auto syncComboOption = [&](QComboBox* combo, const QString& key, bool lc = false) {
		if (combo->currentIndex() > 0)
		{
			QString val = combo->currentText();
			if (lc)
				val = val.toLower().remove(' ');
			optSetValue(tokens, key, val);
		}
		else
			optRemoveKey(tokens, key);
	};
	auto syncSpinOption = [&](QDoubleSpinBox* spin, const QString& key) {
		if (spin->value() != spin->minimum())
			optSetValue(tokens, key, QString::number(spin->value(), 'f', 2));
		else
			optRemoveKey(tokens, key);
	};

	// Text formatting options (tab-aware)
	auto syncEditableComboOption = [&](QComboBox* combo, const QString& key) {
		QString text = combo->currentText().trimmed();
		if (!text.isEmpty() && text.compare("Auto", Qt::CaseInsensitive) != 0)
			optSetValue(tokens, key, text);
		else
			optRemoveKey(tokens, key);
	};

	// Font combo: display "OCR-A"/"OCR-B" but BWIPP expects "OCRA"/"OCRB"
	if (ui.textfontCombo->currentIndex() > 0)
	{
		QString val = ui.textfontCombo->currentText();
		if (val.startsWith("OCR-"))
			val.remove(3, 1);
		optSetValue(tokens, textOptKey("font"), val);
	}
	else
		optRemoveKey(tokens, textOptKey("font"));
	syncEditableComboOption(ui.textsizeCombo, textOptKey("size"));
	syncEditableComboOption(ui.textgapsCombo, textOptKey("gaps"));
	syncComboOption(ui.textdirectionCombo, textOptKey("direction"), true);
	syncComboOption(ui.textxalignCombo, textOptKey("xalign"), true);
	syncComboOption(ui.textyalignCombo, textOptKey("yalign"), true);
	syncEditableComboOption(ui.textxoffsetCombo, textOptKey("xoffset"));
	syncEditableComboOption(ui.textyoffsetCombo, textOptKey("yoffset"));

	if (!ui.alttextEdit->toPlainText().isEmpty())
		optSetValue(tokens, altTextKey(), ui.alttextEdit->toPlainText());
	else
		optRemoveKey(tokens, altTextKey());
	for (auto [combo, subkey] : std::initializer_list<std::pair<QComboBox*, const char*>>{
		{ui.alttextsubspaceCombo, "subspace"},
		{ui.alttextsplitCombo, "split"}})
	{
		QString text = combo->currentText().trimmed();
		if (!text.isEmpty())
			optSetValue(tokens, altTextKey(subkey), text);
		else
			optRemoveKey(tokens, altTextKey(subkey));
	}

	// Border controls
	optRemoveKey(tokens, "showborder");
	optRemoveKey(tokens, "showbearer");
	if (ui.borderBorderRadio->isChecked())
		tokens.append("showborder");
	else if (ui.borderBearerRadio->isChecked())
		tokens.append("showbearer");

	syncSpinOption(ui.borderwidthSpin, "borderwidth");
	syncSpinOption(ui.borderleftSpin, "borderleft");
	syncSpinOption(ui.borderrightSpin, "borderright");
	syncSpinOption(ui.bordertopSpin, "bordertop");
	syncSpinOption(ui.borderbottomSpin, "borderbottom");

	QString newOpts = tokens.join(' ');
	if (ui.optionsEdit->toPlainText() != newOpts)
	{
		ui.optionsEdit->blockSignals(true);
		ui.optionsEdit->setPlainText(newOpts);
		ui.optionsEdit->blockSignals(false);
	}
}

void BarcodeGenerator::updateUIFromOptionsText()
{
	QStringList tokens = ui.optionsEdit->toPlainText().split(' ', Qt::SkipEmptyParts);

	const std::initializer_list<std::pair<QCheckBox*, const char*>> boolOpts = {
		{ui.includetextCheck, "includetext"},
		{ui.guardwhitespaceCheck, "guardwhitespace"},
		{ui.includecheckCheck, "includecheck"},
		{ui.includecheckintextCheck, "includecheckintext"},
		{ui.parseCheck, "parse"},
		{ui.parsefncCheck, "parsefnc"},
		{ui.dottyCheck, "dotty"},
		{ui.cropCheck, "crop"},
	};
	for (const auto& [cb, kw] : boolOpts)
	{
		bool val = optHasKeyword(tokens, QString::fromLatin1(kw));
		if (cb->isChecked() != val)
		{
			cb->blockSignals(true);
			cb->setChecked(val);
			cb->blockSignals(false);
		}
	}

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

	// Sync height slider from options text
	QString hVal = optGetValue(tokens, "height");
	int hInt = hVal.isNull() ? 0 : qBound(20, qRound(hVal.toDouble() * 100), 300);
	if (ui.heightSlider->value() != hInt)
	{
		ui.heightSlider->blockSignals(true);
		ui.heightSlider->setValue(hInt);
		ui.heightSlider->blockSignals(false);
	}
	ui.heightValue->setText(hInt == 0 ? tr("Auto") : QString::number(hInt / 100.0, 'f', 2));

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

	// Sync text formatting from options text
	auto syncComboFromOpt = [&](QComboBox* combo, const QString& key) {
		QString val = optGetValue(tokens, key);
		int idx = 0;
		if (!val.isNull())
		{
			idx = combo->findText(val, Qt::MatchFixedString);
			if (idx == -1)
			{
				// Try matching with spaces removed (e.g. "offleft" -> "Off Left")
				for (int i = 1; i < combo->count(); ++i)
					if (combo->itemText(i).remove(' ').compare(val, Qt::CaseInsensitive) == 0)
					{ idx = i; break; }
			}
			if (idx == -1) idx = 0;
		}
		if (combo->currentIndex() != idx)
		{
			combo->blockSignals(true);
			combo->setCurrentIndex(idx);
			combo->blockSignals(false);
		}
	};
	auto syncSpinFromOpt = [&](QDoubleSpinBox* spin, const QString& key) {
		QString val = optGetValue(tokens, key);
		double dv = val.isNull() ? spin->minimum() : val.toDouble();
		if (spin->value() != dv)
		{
			spin->blockSignals(true);
			spin->setValue(dv);
			spin->blockSignals(false);
		}
	};

	// Text formatting options (tab-aware)
	auto syncEditableComboFromOpt = [&](QComboBox* combo, const QString& key) {
		QString val = optGetValue(tokens, key);
		QString text = val.isNull() ? "Auto" : val;
		if (combo->currentText() != text)
		{
			combo->blockSignals(true);
			combo->setCurrentText(text);
			combo->blockSignals(false);
		}
	};

	// Font combo: BWIPP "OCRA"/"OCRB" -> display "OCR-A"/"OCR-B"
	{
		QString val = optGetValue(tokens, textOptKey("font"));
		int idx = 0;
		if (!val.isNull())
		{
			if (val == "OCRA") val = "OCR-A";
			else if (val == "OCRB") val = "OCR-B";
			idx = ui.textfontCombo->findText(val, Qt::MatchFixedString);
			if (idx == -1) idx = 0;
		}
		if (ui.textfontCombo->currentIndex() != idx)
		{
			ui.textfontCombo->blockSignals(true);
			ui.textfontCombo->setCurrentIndex(idx);
			ui.textfontCombo->blockSignals(false);
		}
	}
	syncEditableComboFromOpt(ui.textsizeCombo, textOptKey("size"));
	syncEditableComboFromOpt(ui.textgapsCombo, textOptKey("gaps"));
	syncComboFromOpt(ui.textdirectionCombo, textOptKey("direction"));
	syncComboFromOpt(ui.textxalignCombo, textOptKey("xalign"));
	syncComboFromOpt(ui.textyalignCombo, textOptKey("yalign"));
	syncEditableComboFromOpt(ui.textxoffsetCombo, textOptKey("xoffset"));
	syncEditableComboFromOpt(ui.textyoffsetCombo, textOptKey("yoffset"));

	QString altVal = optGetValue(tokens, altTextKey());
	QString altText = altVal.isNull() ? QString() : altVal;
	if (ui.alttextEdit->toPlainText() != altText)
	{
		ui.alttextEdit->blockSignals(true);
		ui.alttextEdit->setPlainText(altText);
		ui.alttextEdit->blockSignals(false);
	}
	for (auto [combo, subkey] : std::initializer_list<std::pair<QComboBox*, const char*>>{
		{ui.alttextsubspaceCombo, "subspace"},
		{ui.alttextsplitCombo, "split"}})
	{
		QString val = optGetValue(tokens, altTextKey(subkey));
		QString text = val.isNull() ? QString() : val;
		if (combo->currentText() != text)
		{
			combo->blockSignals(true);
			combo->setCurrentText(text);
			combo->blockSignals(false);
		}
	}

	// Sync border from options text
	bool hasBorder = optHasKeyword(tokens, "showborder");
	bool hasBearer = optHasKeyword(tokens, "showbearer");
	if (hasBorder && !ui.borderBorderRadio->isChecked())
	{
		ui.borderBorderRadio->blockSignals(true);
		ui.borderBorderRadio->setChecked(true);
		ui.borderBorderRadio->blockSignals(false);
	}
	else if (hasBearer && !ui.borderBearerRadio->isChecked())
	{
		ui.borderBearerRadio->blockSignals(true);
		ui.borderBearerRadio->setChecked(true);
		ui.borderBearerRadio->blockSignals(false);
	}
	else if (!hasBorder && !hasBearer && !ui.borderNoneRadio->isChecked())
	{
		ui.borderNoneRadio->blockSignals(true);
		ui.borderNoneRadio->setChecked(true);
		ui.borderNoneRadio->blockSignals(false);
	}

	syncSpinFromOpt(ui.borderwidthSpin, "borderwidth");
	syncSpinFromOpt(ui.borderleftSpin, "borderleft");
	syncSpinFromOpt(ui.borderrightSpin, "borderright");
	syncSpinFromOpt(ui.bordertopSpin, "bordertop");
	syncSpinFromOpt(ui.borderbottomSpin, "borderbottom");
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

void BarcodeGenerator::setControlsEnabled(bool enabled)
{
	ui.codeEdit->setEnabled(enabled);
	ui.optionsEdit->setEnabled(enabled);
	ui.includetextCheck->setEnabled(enabled);
	ui.cropCheck->setEnabled(enabled);
	ui.inkspreadLabel->setEnabled(enabled);
	ui.inkspreadSlider->setEnabled(enabled);
	ui.inkspreadValue->setEnabled(enabled);
	ui.bgColorButton->setEnabled(enabled);
	ui.lnColorButton->setEnabled(enabled);
	ui.txtColorButton->setEnabled(enabled);
	ui.textBox->setEnabled(enabled);
	ui.borderBox->setEnabled(enabled);
	ui.okButton->setEnabled(enabled);
	if (!enabled)
	{
		ui.guardwhitespaceCheck->setEnabled(false);
		ui.includecheckCheck->setEnabled(false);
		ui.includecheckintextCheck->setEnabled(false);
		ui.parseCheck->setEnabled(false);
		ui.parsefncCheck->setEnabled(false);
		ui.dottyCheck->setEnabled(false);
		ui.formatLabel->setEnabled(false);
		ui.formatCombo->setEnabled(false);
		ui.eccLabel->setEnabled(false);
		ui.eccCombo->setEnabled(false);
		ui.heightLabel->setEnabled(false);
		ui.heightSlider->setEnabled(false);
		ui.heightValue->setEnabled(false);
	}
}

void BarcodeGenerator::mousePressEvent(QMouseEvent* event)
{
	QWidget* fw = focusWidget();
	if (fw == ui.optionsEdit || fw == ui.optionsEdit->viewport() ||
	    fw == ui.alttextEdit || fw == ui.alttextEdit->viewport())
		setFocus();
	QDialog::mousePressEvent(event);
}

bool BarcodeGenerator::eventFilter(QObject* obj, QEvent* event)
{
	// Expand text fields to 3 lines on focus, collapse on blur
	auto expandField = [this](QPlainTextEdit* field, QEvent* ev, bool syncOnBlur) {
		static const char* kConn = "expandConn";
		int collapsedHeight = ui.codeEdit->sizeHint().height();
		auto fitHeight = [field, collapsedHeight]() {
			int docLines = field->document()->size().toSize().height();
			if (docLines <= 1)
			{
				field->setMinimumHeight(collapsedHeight);
				field->setMaximumHeight(collapsedHeight);
				return;
			}
			QFontMetrics fm(field->font());
			int lineHeight = fm.lineSpacing();
			int margins = field->contentsMargins().top() + field->contentsMargins().bottom()
				+ field->document()->documentMargin() * 2 + 4;
			int maxHeight = 3 * lineHeight + margins;
			int fitted = qMin(docLines * lineHeight + margins, maxHeight);
			field->setMinimumHeight(fitted);
			field->setMaximumHeight(fitted);
		};
		if (ev->type() == QEvent::FocusIn)
		{
			fitHeight();
			auto conn = connect(field, &QPlainTextEdit::textChanged, field, fitHeight);
			field->setProperty(kConn, QVariant::fromValue(conn));
			QTimer::singleShot(0, this, [this]() { enqueuePaintBarcode(0); });
		}
		else if (ev->type() == QEvent::FocusOut)
		{
			auto conn = field->property(kConn).value<QMetaObject::Connection>();
			disconnect(conn);
			QTimer::singleShot(0, this, [this, field, collapsedHeight, syncOnBlur]() {
				field->setMaximumHeight(collapsedHeight);
				field->setMinimumHeight(collapsedHeight);
				if (syncOnBlur)
					updateUIFromOptionsText();
			});
		}
	};
	if (obj == ui.optionsEdit)
		expandField(ui.optionsEdit, event, true);
	if (obj == ui.alttextEdit)
		expandField(ui.alttextEdit, event, false);

	if (event->type() == QEvent::KeyPress)
	{
		QKeyEvent* ke = static_cast<QKeyEvent*>(event);
		bool isOptions = (obj == ui.optionsEdit || obj == ui.optionsEdit->viewport());
		bool isAlttext = (obj == ui.alttextEdit || obj == ui.alttextEdit->viewport());

		// Block Enter in options field; substitute CR character in alttext
		if ((isOptions || isAlttext) && (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter))
		{
			if (isAlttext)
			{
				QString split = ui.alttextsplitCombo->currentText();
				if (!split.isEmpty())
					ui.alttextEdit->textCursor().insertText(split);
			}
			return true;
		}
		// Substitute SP character in alttext
		if (isAlttext && ke->key() == Qt::Key_Space)
		{
			QString sub = ui.alttextsubspaceCombo->currentText();
			if (!sub.isEmpty())
				ui.alttextEdit->textCursor().insertText(sub);
			return true;
		}
		// Block spaces in the SP/CR combos
		if ((obj == ui.alttextsubspaceCombo->lineEdit() ||
		     obj == ui.alttextsplitCombo->lineEdit()) &&
		    ke->key() == Qt::Key_Space)
			return true;
	}
	return QDialog::eventFilter(obj, event);
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

	// Write PS file synchronously
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
		addAttr("bwipp-options", ui.optionsEdit->toPlainText());
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

void BarcodeGenerator::syncOptionsUI()
{
	updateUIFromOptionsText();
	enqueuePaintBarcode(0);
}

QString BarcodeGenerator::buildPSCommand()
{
	QString opts = ui.optionsEdit->toPlainText().replace('\n', ' ').replace('\r', ' ');

	// Only append default colors for values NOT already in the options string
	QStringList tokens = opts.split(' ', Qt::SkipEmptyParts);

	// "crop" controls import bounding — not a BWIPP option, so strip it
	bool crop = optHasKeyword(tokens, "crop");
	optRemoveKey(tokens, "crop");

	if (optGetValue(tokens, "barcolor").isNull())
		tokens.append("barcolor=" + lnColor.name().replace('#', "").toUpper());
	if (!crop && optGetValue(tokens, "backgroundcolor").isNull())
	{
		tokens.append("showbackground");
		tokens.append("backgroundcolor=" + bgColor.name().replace('#', "").toUpper());
	}
	if (optGetValue(tokens, "textcolor").isNull())
		tokens.append("textcolor=" + txtColor.name().replace('#', "").toUpper());
	opts = tokens.join(' ');

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
	QString comm("100 100 moveto <%1> <%2> /%3 /uk.co.terryburton.bwipp findresource exec\n");
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
	QSize sz = ui.sampleLabel->size();
	thread.render(buildPSCommand(), sz.width(), sz.height());
}

void BarcodeGenerator::resetButton_clicked()
{
	bcComboChanged();
}
