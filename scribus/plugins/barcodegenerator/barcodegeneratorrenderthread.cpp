/*
For general Scribus (>=1.3.2) copyright and licensing information please refer
to the COPYING file provided with the program. Following this notice may exist
a copyright and/or license notice that predates the release of Scribus 1.3.2
for which a new license (GPL+exception) is in place.
*/


#include "barcodegeneratorrenderthread.h"
#include "scpaths.h"
#include "scribuscore.h"
#include "util_ghostscript.h"

#include <QDir>
#include <QRegularExpression>
#include <QTextStream>
#include <QThread>


BarcodeGeneratorRenderThread::BarcodeGeneratorRenderThread(QObject *parent) : QThread(parent)
{
	restart = false;
	abort = false;
}

BarcodeGeneratorRenderThread::~BarcodeGeneratorRenderThread()
{
	mutex.lock();
	abort = true;
	condition.wakeOne();
	mutex.unlock();
	wait();
}

void BarcodeGeneratorRenderThread::render(const QString& psCommand, int previewWidth, int previewHeight)
{
	QMutexLocker locker(&mutex);

	this->psCommand = psCommand;
	this->previewWidth = previewWidth;
	this->previewHeight = previewHeight;

	if (!isRunning()) {
		start(LowPriority);
	} else {
		restart = true;
		condition.wakeOne();
	}
}

static QString parseBwippError(const QString& fileStdErr)
{
	if (!QFile::exists(fileStdErr))
		return QString();
	QFile f(fileStdErr);
	if (!f.open(QIODevice::ReadOnly))
		return QString();
	QTextStream ts(&f);
	QString err = ts.readAll();
	f.close();
	QRegularExpression rx("[\\r\\n]+BWIPP ERROR: [^\\s]+ (.*)[\\r\\n$]+", QRegularExpression::InvertedGreedinessOption);
	QRegularExpressionMatch match = rx.match(err);
	return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

void BarcodeGeneratorRenderThread::run()
{
	QString pngFile = QDir::toNativeSeparators(ScPaths::tempFileDir() + "bcode.png");
	QString psFile = QDir::toNativeSeparators(ScPaths::tempFileDir() + "bcode.ps");
	QString fileStdErr = QDir::toNativeSeparators(ScPaths::tempFileDir() + "bcode.err");
	QString fileStdOut = QDir::toNativeSeparators(ScPaths::tempFileDir() + "bcode.out");

	forever
	{
		mutex.lock();
		QString psCommand = this->psCommand;
		int pvWidth = this->previewWidth;
		int pvHeight = this->previewHeight;
		mutex.unlock();

		QFile f(psFile);
		f.open(QIODevice::WriteOnly);
		QTextStream ts(&f);
		ts << psCommand;
		f.close();

		QString errorMsg;

		// Pass 1: Get bounding box of the barcode at native size
		// Use PageOffset to shift barcode away from edges so descending
		// text below the moveto origin isn't clipped
		static const int bboxOffset = 3000;
		QString bboxPs = psCommand;
		int insertPos = bboxPs.indexOf('\n') + 1;
		bboxPs.insert(insertPos, QString("<< /PageOffset [%1 %2] >> setpagedevice\n")
			.arg(bboxOffset).arg(bboxOffset));
		{
			QFile bf(psFile);
			bf.open(QIODevice::WriteOnly);
			QTextStream bts(&bf);
			bts << bboxPs;
		}
		QStringList bboxArgs;
		bboxArgs.append("-dDEVICEWIDTHPOINTS=10000");
		bboxArgs.append("-dDEVICEHEIGHTPOINTS=10000");
		bboxArgs.append(psFile);
		QFile::remove(fileStdErr);
		int gs = callGS(bboxArgs, "bbox", fileStdErr, fileStdOut);

		double bboxX1 = 0, bboxY1 = 0, bboxX2 = 0, bboxY2 = 0;
		bool bboxOk = false;
		if (gs == 0 && QFile::exists(fileStdErr))
		{
			QFile ef(fileStdErr);
			if (ef.open(QIODevice::ReadOnly))
			{
				QTextStream ets(&ef);
				QString bboxOutput = ets.readAll();
				ef.close();
				// Parse %%HiResBoundingBox: x1 y1 x2 y2
				QRegularExpression rx("%%HiResBoundingBox:\\s+([\\d.]+)\\s+([\\d.]+)\\s+([\\d.]+)\\s+([\\d.]+)");
				QRegularExpressionMatch match = rx.match(bboxOutput);
				if (match.hasMatch())
				{
					bboxX1 = match.captured(1).toDouble() - bboxOffset;
					bboxY1 = match.captured(2).toDouble() - bboxOffset;
					bboxX2 = match.captured(3).toDouble() - bboxOffset;
					bboxY2 = match.captured(4).toDouble() - bboxOffset;
					bboxOk = (bboxX2 > bboxX1 && bboxY2 > bboxY1);
				}
				// Also check for BWIPP error in the bbox output
				if (!bboxOk)
				{
					QString bwippErr = parseBwippError(fileStdErr);
					if (!bwippErr.isEmpty())
						errorMsg = bwippErr;
				}
			}
		}

		if (!bboxOk && errorMsg.isEmpty())
			errorMsg = "Barcode incomplete";

		bool retval = false;
		if (bboxOk)
		{
			double bcWidth = bboxX2 - bboxX1;
			double bcHeight = bboxY2 - bboxY1;

			// Calculate scale to fit 90% of the preview pane
			double targetW = pvWidth * 0.9;
			double targetH = pvHeight * 0.9;
			double scale = qMin(targetW / bcWidth, targetH / bcHeight);
			if (scale <= 0)
				scale = 1.0;

			// Final image dimensions in pixels (at 72 DPI, 1pt = 1px)
			int imgWidth = pvWidth;
			int imgHeight = pvHeight;

			// Translation to center the scaled barcode
			double scaledW = bcWidth * scale;
			double scaledH = bcHeight * scale;
			double tx = (pvWidth - scaledW) / 2.0 - bboxX1 * scale;
			double ty = (pvHeight - scaledH) / 2.0 - bboxY1 * scale;

			// Pass 2: Render scaled and centered
			// Inject scale and translate into the PS before the barcode command
			QString scaledPs = psCommand;
			// Insert transform after the resource loading, before the "moveto"
			QString transform = QString("%1 %2 translate %3 %3 scale\n")
				.arg(tx, 0, 'f', 2)
				.arg(ty, 0, 'f', 2)
				.arg(scale, 0, 'f', 4);
			// Find the moveto line and prepend the transform
			int movetoPos = scaledPs.lastIndexOf(" moveto ");
			if (movetoPos >= 0)
			{
				// Find the start of the line containing moveto
				int lineStart = scaledPs.lastIndexOf('\n', movetoPos) + 1;
				scaledPs.insert(lineStart, transform);
			}

			QFile f2(psFile);
			f2.open(QIODevice::WriteOnly);
			QTextStream ts2(&f2);
			ts2 << scaledPs;
			f2.close();

			QStringList renderArgs;
			renderArgs.append(QString("-dDEVICEWIDTHPOINTS=%1").arg(imgWidth));
			renderArgs.append(QString("-dDEVICEHEIGHTPOINTS=%1").arg(imgHeight));
			renderArgs.append("-r72");
			renderArgs.append(QString("-sOutputFile=%1").arg(pngFile));
			renderArgs.append(psFile);
			QFile::remove(pngFile);
			gs = callGS(renderArgs, QString(), fileStdErr, fileStdOut);
			retval = gs == 0 && QFile::exists(pngFile);

			if (!retval)
			{
				errorMsg = "Barcode incomplete";
				QString bwippErr = parseBwippError(fileStdErr);
				if (!bwippErr.isEmpty())
					errorMsg = bwippErr;
			}
		}

		if (abort)
			return;

		if (!restart)
			emit renderedImage(errorMsg);

		// Go to sleep unless restarting
		mutex.lock();
		if (!restart)
			condition.wait(&mutex);
		restart = false;
		mutex.unlock();
	}

}

