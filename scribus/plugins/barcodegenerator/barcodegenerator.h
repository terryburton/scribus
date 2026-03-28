/*
For general Scribus (>=1.3.2) copyright and licensing information please refer
to the COPYING file provided with the program. Following this notice may exist
a copyright and/or license notice that predates the release of Scribus 1.3.2
for which a new license (GPL+exception) is in place.
*/

#ifndef BARCODEGENERATOR_H
#define BARCODEGENERATOR_H

#include "ui_barcodegenerator.h"
#include "barcodegeneratorrenderthread.h"
#include "bwipp/postscriptbarcode.hpp"

#include <QDialog>
#include <QLabel>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <optional>

#include "sccolor.h"

class QButtonGroup;
class HelpBrowser;

class PageItem;

struct BarcodeComboConfig {
	QString name;
	QString key;
	QStringList values;
};

struct BarcodeEncoderUI {
	bool enabled = true;
	int order = 0;
	QString desc;
	QString exam;
	QString exop;
	BarcodeComboConfig combo1;
	BarcodeComboConfig combo2;
	bool includetext = false;
	bool guardwhitespace = false;
	bool includecheck = false;
	bool includecheckintext = false;
	bool parse = false;
	bool parsefnc = false;
	bool dotty = false;
	bool dottyForced = false;
	bool height = false;
	bool bearer = false;
	bool fixedtext = false;
};

struct BarcodeFamilyUI {
	bool enabled = true;
	int order = 0;
	QString desc;
};

/*! \brief One Barcode Entity.
\author Petr Vanek <petr@yarpen.cz>
 */
class BarcodeType
{
	public:
		//! \brief Constructor provided for QMap initialization only.
		BarcodeType(){};
		/*! \brief Setup the Barcode entity.
		\param cmd a Postsript command for given BC type
		\param exa an example of the contents
		\param exaop an example of the options */
		BarcodeType(const QString &cmd, const QString &exa, const QString &exaop);
		~BarcodeType(){};
		//! \brief PostScript encoder
		QString command;
		//! \brief BC example contents
		QString exampleContents;
		//! \brief BC example options
		QString exampleOptions;
};


//! \brief Type for BC name/BC type mapping.
using BarcodeMap = QMap<QString, BarcodeType>;


/*! \brief Active tasks for BC GUI.
It's inherited from BarcodeGeneratorBase() class which is created
by uic from designer. Don't change anything in BarcodeGeneratorBase
manually! It will be overwritten automatically by uic.
\author Petr Vanek <petr@yarpen.cz>
*/
class BarcodeGenerator : public QDialog
{
	Q_OBJECT

	public:
		/*! \brief Create modal BC dialog.
		\param parent Parent of the dialog.
		\param name name od the QObject to debug */
		BarcodeGenerator(QWidget* parent = nullptr, const char* name = 0);
		//! \brief Clean up temporary files and resources.
		~BarcodeGenerator();

		/*! \brief Pre-populate the dialog from a barcode item's stored attributes.
			\param item A PageItem with bwipp-* ObjectAttributes */
		void loadFromItem(PageItem* item);

		/*! \brief Pre-populate the dialog from a parameter map (scripter path).
			\param params Map with bwipp-encoder, bwipp-content, bwipp-options */
		void loadFromParams(const QMap<QString, QString>& params);

		/*! \brief Generate barcode and optionally replace an existing item.
			Builds the PostScript, imports it, attaches bwipp-* attributes,
			and (if replaceItem is set) swaps the old item preserving geometry.
			This is the common path used by the dialog OK button, attribute
			edits, and the scripter.
			\param replaceItem Item to replace, or nullptr for new placement
			\param placeX X coordinate for scripted placement (-1 for interactive)
			\param placeY Y coordinate for scripted placement (-1 for interactive) */
		bool generateBarcode(PageItem* replaceItem = nullptr, double placeX = -1, double placeY = -1);

	protected:
		bool eventFilter(QObject* obj, QEvent* event) override;
		void mousePressEvent(QMouseEvent* event) override;

		//! GUI namespace content. See designer.
		Ui::BarcodeGeneratorBase ui;

		//! \brief BC/BC type mapping. QMap keys are used as BC names.
		BarcodeMap map;

		QTimer* paintBarcodeTimer { nullptr };

		//! \brief Per-encoder UI configuration from barcode_ui.json.
		QHash<QString, BarcodeEncoderUI> encoderUI;
		//! \brief Per-family UI configuration from barcode_ui.json.
		QHash<QString, BarcodeFamilyUI> familyUI;
		//! \brief List of barcode families.
		QList<QString> familyList;
		//! \brief Family name to encoder display names.
		QHash<QString, QStringList> familyItems;

		//! \brief Color of the BC lines.
		ScColor lnColor;
		//! \brief Color of the BC font.
		ScColor txtColor;
		//! \brief Background color of the BC.
		ScColor bgColor;

		/*! \brief Create color preview.
		Used for Color box feedback.
		\param l A pointer to the sample QLabel
		\param c A color to fill */
		void paintColorSample(QLabel *l, const ScColor & c);
		void updateOptions();
		void updateOptionsTextFromUI();
		void updateUIFromOptionsText();
		//! \brief Item being edited (nullptr when creating new barcode)
		PageItem* m_editItem {nullptr};

	private:
		int m_activeTextTab { 1 };
		QButtonGroup* m_textTabGroup { nullptr };
		QString textOptKey(const QString& suffix) const;
		QString altTextKey(const QString& subkey = QString()) const;

		std::optional<bwipp::BWIPP> m_bwipp;
		HelpBrowser* m_helpBrowser {nullptr};
		void loadUIConfig(const QString& path);
		void showHelpBrowser(const QString& file);
		void enqueuePaintBarcode(int);
		QString buildPSCommand();
		BarcodeGeneratorRenderThread thread;
		QTimer* syncOptionsUITimer { nullptr };
		QTimer* syncOptionsTextTimer { nullptr };

		/*! \brief Shared UI population from encoder/content/options */
		void loadBarcode(const QString& encoder, const QString& content, const QString& options);
		/*! \brief Replace or append key=value in the options text field */
		void updateOptionValue(const QString& key, const QString& value);
		/*! \brief Ensure a boolean option is present in the options text field */
		void ensureOptionPresent(const QString& key);

		void setControlsEnabled(bool enabled);
		void updateTextControlsEnabled();

	protected slots:
		void paintBarcode();
		void updatePreview(const QString&);
		void bcFamilyComboChanged();
		void bcComboChanged();
		void bcComboChanged(int);
		void bgColorButton_pressed();
		void lnColorButton_pressed();
		void txtColorButton_pressed();
		void codeEdit_textChanged(const QString& s);
		void resetButton_clicked();
		void helpSymbologiesButton_pressed();
		void helpOptionsButton_pressed();
		void okButton_pressed();
		void cancelButton_pressed();
	private slots:
		void syncOptionsUI();
};

#endif
