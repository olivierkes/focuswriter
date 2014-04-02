/***********************************************************************
 *
 * Copyright (C) 2009, 2010, 2011, 2012, 2013, 2014 Graeme Gott <graeme@gottcode.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 ***********************************************************************/

#include "theme.h"

#include "session.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QPainter>
#include <QSettings>
#include <QUrl>

//-----------------------------------------------------------------------------

// Exported by QtGui
void qt_blurImage(QPainter* p, QImage& blurImage, qreal radius, bool quality, bool alphaOnly, int transposed = 0);

//-----------------------------------------------------------------------------

bool compareFiles(const QString& filename1, const QString& filename2)
{
	// Compare sizes
	QFile file1(filename1);
	QFile file2(filename2);
	if (file1.size() != file2.size()) {
		return false;
	}

	// Compare contents
	bool equal = true;
	if (file1.open(QFile::ReadOnly) && file2.open(QFile::ReadOnly)) {
		while (!file1.atEnd()) {
			if (file1.read(1000) != file2.read(1000)) {
				equal = false;
				break;
			}
		}
		file1.close();
		file2.close();
	} else {
		equal = false;
	}
	return equal;
}

namespace
{
	QString copyImage(const QString& image)
	{
		// Check if already copied
		QDir images(Theme::path() + "/Images/");
		QStringList filenames = images.entryList(QDir::Files);
		foreach (const QString& filename, filenames) {
			if (compareFiles(image, images.filePath(filename))) {
				return filename;
			}
		}

		// Find file name
		QString base = QCryptographicHash::hash(image.toUtf8(), QCryptographicHash::Sha1).toHex();
		QString suffix = QFileInfo(image).suffix().toLower();
		QString filename = QString("%1.%2").arg(base, suffix);

		// Handle file name collisions
		int id = 0;
		while (images.exists(filename)) {
			id++;
			filename = QString("%1-%2.%3").arg(base).arg(id).arg(suffix);
		}

		QFile::copy(image, images.filePath(filename));
		return filename;
	}
}

//-----------------------------------------------------------------------------

QString Theme::m_path;

//-----------------------------------------------------------------------------

Theme::ThemeData::ThemeData(const QString& name_, bool create) :
	name(name_),
	background_type(0, 5),
	foreground_opacity(0, 100),
	foreground_width(500, 9999),
	foreground_rounding(0, 100),
	foreground_margin(1, 250),
	foreground_padding(0, 250),
	foreground_position(0, 3),
	blur_enabled(false),
	blur_radius(1, 128),
	shadow_enabled(false),
	shadow_offset(0, 128),
	shadow_radius(1, 128),
	line_spacing(50, 1000),
	paragraph_spacing_above(0, 1000),
	paragraph_spacing_below(0, 1000),
	tab_width(1, 1000)
{
	if (name.isEmpty() && create) {
		QString untitled;
		int count = 0;
		do {
			count++;
			untitled = Theme::tr("Untitled %1").arg(count);
		} while (QFile::exists(Theme::filePath(untitled)));
		name = untitled;
	}
}

//-----------------------------------------------------------------------------

Theme::Theme(const QString& name, bool create)
{
	d = new ThemeData(name, create);
	forgetChanges();
}

//-----------------------------------------------------------------------------

Theme::~Theme()
{
	saveChanges();
}

//-----------------------------------------------------------------------------

void Theme::copyBackgrounds()
{
	QDir dir(path() + "/Images");
	QStringList images;

	// Copy images
	QStringList themes = QDir(path(), "*.theme").entryList(QDir::Files);
	foreach (const QString& theme, themes) {
		QSettings settings(path() + "/" + theme, QSettings::IniFormat);
		QString background_path = settings.value("Background/Image").toString();
		QString background_image = settings.value("Background/ImageFile").toString();
		if (background_path.isEmpty() && background_image.isEmpty()) {
			continue;
		}
		if (!background_path.isEmpty() && (background_image.isEmpty() || !dir.exists(background_image))) {
			background_image = copyImage(background_path);
			settings.setValue("Background/ImageFile", background_image);
		}
		images.append(background_image);
	}

	// Delete unused images
	QStringList files = dir.entryList(QDir::Files);
	foreach (const QString& file, files) {
		if (!images.contains(file)) {
			QFile::remove(path() + "/Images/" + file);
		}
	}
}

//-----------------------------------------------------------------------------

QImage Theme::render(const QSize& background, QRect& foreground) const
{
	// Create image
	QImage image(background, QImage::Format_ARGB32_Premultiplied);
	image.fill(backgroundColor());

	QPainter painter(&image);
	painter.setPen(Qt::NoPen);

	// Draw background image
	if (backgroundType() > 1) {
		QImageReader source(backgroundImage());
		QSize scaled = source.size();
		switch (backgroundType()) {
		case 3:
			// Stretched
			scaled.scale(background, Qt::IgnoreAspectRatio);
			break;
		case 4:
			// Scaled
			scaled.scale(background, Qt::KeepAspectRatio);
			break;
		case 5:
			// Zoomed
			scaled.scale(background, Qt::KeepAspectRatioByExpanding);
			break;
		default:
			// Centered
			break;
		}
		source.setScaledSize(scaled);
		painter.drawImage((background.width() - scaled.width()) / 2, (background.height() - scaled.height()) / 2, source.read());
	} else if (backgroundType() == 1) {
		// Tiled
		painter.fillRect(image.rect(), QImage(backgroundImage()));
	}

	// Determine foreground rectangle
	foreground = foregroundRect(background);

	// Set clipping for rounded themes
	QPainterPath path;
	int rounding = foregroundRounding();
	if (rounding) {
		painter.setRenderHint(QPainter::Antialiasing);
		path.addRoundedRect(foreground, rounding, rounding);
		painter.setClipPath(path);
	} else {
		path.addRect(foreground);
	}

	// Blur behind foreground
	if (blurEnabled()) {
		QImage blurred = image.copy(foreground);

		painter.save();
		painter.translate(foreground.x(), foreground.y());
		qt_blurImage(&painter, blurred, blurRadius() * 2, true, false);
		painter.restore();
	}

	// Draw drop shadow
	int shadow_radius = shadowEnabled() ? shadowRadius() : 0;
	if (shadow_radius) {
		QImage copy = image.copy(foreground);

		QImage shadow(background, QImage::Format_ARGB32_Premultiplied);
		shadow.fill(Qt::transparent);

		QPainter shadow_painter(&shadow);
		shadow_painter.setRenderHint(QPainter::Antialiasing);
		shadow_painter.setPen(Qt::NoPen);
		shadow_painter.translate(0, shadowOffset());
		shadow_painter.fillPath(path, shadowColor());
		shadow_painter.end();

		painter.save();
		painter.setClipping(false);
		qt_blurImage(&painter, shadow, shadow_radius * 2, true, false);
		painter.setClipping(rounding);
		painter.restore();

		painter.drawImage(foreground.x(), foreground.y(), copy);
	}

	// Draw foreground
	QColor color = foregroundColor();
	color.setAlpha(foregroundOpacity() * 2.55f);
	painter.fillRect(foreground, color);

	return image;
}

//-----------------------------------------------------------------------------

QString Theme::filePath(const QString& theme)
{
	return m_path + "/" + QUrl::toPercentEncoding(theme, " ") + ".theme";
}

//-----------------------------------------------------------------------------

QString Theme::iconPath(const QString& theme)
{
	return m_path + "/" + QUrl::toPercentEncoding(theme, " ") + ".png";
}

//-----------------------------------------------------------------------------

void Theme::setName(const QString& name)
{
	if (d->name != name) {
		QStringList files = QDir(Session::path(), "*.session").entryList(QDir::Files);
		files.prepend("");
		foreach (const QString& file, files) {
			Session session(file);
			if (session.theme() == d->name) {
				session.setTheme(name);
			}
		}

		QFile::remove(filePath(d->name));
		QFile::remove(iconPath(d->name));
		setValue(d->name, name);
	}
}

//-----------------------------------------------------------------------------

void Theme::setBackgroundImage(const QString& path)
{
	if (d->background_path != path) {
		setValue(d->background_path, path);
		if (!d->background_path.isEmpty()) {
			d->background_image = copyImage(d->background_path);
		} else {
			d->background_image.clear();
		}
	}
}

//-----------------------------------------------------------------------------

QRect Theme::foregroundRect(const QSize& size) const
{
	int margin = d->foreground_margin;
	int x = 0;
	int y = margin;
	int width = std::min(d->foreground_width.value(), size.width() - (margin * 2));
	int height = size.height() - (margin * 2);

	switch (d->foreground_position) {
	case 0:
		// Left
		x = margin;
		break;
	case 2:
		// Right
		x = size.width() - margin - width;
		break;
	case 3:
		// Stretched
		x = margin;
		width = size.width() - (margin * 2);
		break;
	case 1:
	default:
		// Centered
		x = (size.width() - width) / 2;
		break;
	};

	return QRect(x, y, width, height);
}

//-----------------------------------------------------------------------------

bool Theme::operator==(const Theme& theme) const
{
	return (d->name == theme.d->name)

		&& (d->background_type == theme.d->background_type)
		&& (d->background_color == theme.d->background_color)
		&& (d->background_path == theme.d->background_path)
		&& (d->background_image == theme.d->background_image)

		&& (d->foreground_color == theme.d->foreground_color)
		&& (d->foreground_opacity == theme.d->foreground_opacity)
		&& (d->foreground_width == theme.d->foreground_width)
		&& (d->foreground_rounding == theme.d->foreground_rounding)
		&& (d->foreground_margin == theme.d->foreground_margin)
		&& (d->foreground_padding == theme.d->foreground_padding)
		&& (d->foreground_position == theme.d->foreground_position)

		&& (d->blur_enabled == theme.d->blur_enabled)
		&& (d->blur_radius == theme.d->blur_radius)

		&& (d->shadow_enabled == theme.d->shadow_enabled)
		&& (d->shadow_offset == theme.d->shadow_offset)
		&& (d->shadow_radius == theme.d->shadow_radius)
		&& (d->shadow_color == theme.d->shadow_color)

		&& (d->text_color == theme.d->text_color)
		&& (d->text_font == theme.d->text_font)
		&& (d->misspelled_color == theme.d->misspelled_color)

		&& (d->indent_first_line == theme.d->indent_first_line)
		&& (d->line_spacing == theme.d->line_spacing)
		&& (d->paragraph_spacing_above == theme.d->paragraph_spacing_above)
		&& (d->paragraph_spacing_below == theme.d->paragraph_spacing_below)
		&& (d->tab_width == theme.d->tab_width);
}

//-----------------------------------------------------------------------------

void Theme::reload()
{
	if (d->name.isEmpty()) {
		return;
	}

	QSettings settings(filePath(d->name), QSettings::IniFormat);

	// Load background settings
	d->background_type = settings.value("Background/Type", 0).toInt();
	d->background_color = settings.value("Background/Color", "#cccccc").toString();
	d->background_path = settings.value("Background/Image").toString();
	d->background_image = settings.value("Background/ImageFile").toString();
	if (!d->background_path.isEmpty() && d->background_image.isEmpty()) {
		setValue(d->background_image, copyImage(d->background_path));
	}

	// Load foreground settings
	d->foreground_color = settings.value("Foreground/Color", "#cccccc").toString();
	d->foreground_opacity = settings.value("Foreground/Opacity", 100).toInt();
	d->foreground_width = settings.value("Foreground/Width", 700).toInt();
	d->foreground_rounding = settings.value("Foreground/Rounding", 0).toInt();
	d->foreground_margin = settings.value("Foreground/Margin", 65).toInt();
	d->foreground_padding = settings.value("Foreground/Padding", 0).toInt();
	d->foreground_position = settings.value("Foreground/Position", 1).toInt();

	d->blur_enabled = settings.value("ForegroundBlur/Enabled", false).toBool();
	d->blur_radius = settings.value("ForegroundBlur/Radius", 32).toInt();

	d->shadow_enabled = settings.value("ForegroundShadow/Enabled", false).toBool();
	d->shadow_color = settings.value("ForegroundShadow/Color", "#000000").toString();
	d->shadow_radius = settings.value("ForegroundShadow/Radius", 8).toInt();
	d->shadow_offset = settings.value("ForegroundShadow/Offset", 2).toInt();

	// Load text settings
	d->text_color = settings.value("Text/Color", "#000000").toString();
	d->text_font.fromString(settings.value("Text/Font", QFont("Times New Roman").toString()).toString());
	d->misspelled_color = settings.value("Text/Misspelled", "#ff0000").toString();

	// Load spacings
	d->indent_first_line = settings.value("Spacings/IndentFirstLine", false).toBool();
	d->line_spacing = settings.value("Spacings/LineSpacing", 100).toInt();
	d->paragraph_spacing_above = settings.value("Spacings/ParagraphAbove", 0).toInt();
	d->paragraph_spacing_below = settings.value("Spacings/ParagraphBelow", 0).toInt();
	d->tab_width = settings.value("Spacings/TabWidth", 48).toInt();
}

//-----------------------------------------------------------------------------

void Theme::write()
{
	if (d->name.isEmpty()) {
		return;
	}

	QSettings settings(filePath(d->name), QSettings::IniFormat);

	// Store background settings
	settings.setValue("Background/Type", d->background_type.value());
	settings.setValue("Background/Color", d->background_color.name());
	if (!d->background_path.isEmpty()) {
		settings.setValue("Background/Image", d->background_path);
	}
	settings.setValue("Background/ImageFile", d->background_image);

	// Store foreground settings
	settings.setValue("Foreground/Color", d->foreground_color.name());
	settings.setValue("Foreground/Opacity", d->foreground_opacity.value());
	settings.setValue("Foreground/Width", d->foreground_width.value());
	settings.setValue("Foreground/Rounding", d->foreground_rounding.value());
	settings.setValue("Foreground/Margin", d->foreground_margin.value());
	settings.setValue("Foreground/Padding", d->foreground_padding.value());
	settings.setValue("Foreground/Position", d->foreground_position.value());

	settings.setValue("ForegroundBlur/Enabled", d->blur_enabled);
	settings.setValue("ForegroundBlur/Radius", d->blur_radius.value());

	settings.setValue("ForegroundShadow/Enabled", d->shadow_enabled);
	settings.setValue("ForegroundShadow/Color", d->shadow_color.name());
	settings.setValue("ForegroundShadow/Radius", d->shadow_radius.value());
	settings.setValue("ForegroundShadow/Offset", d->shadow_offset.value());

	// Store text settings
	settings.setValue("Text/Color", d->text_color.name());
	settings.setValue("Text/Font", d->text_font.toString());
	settings.setValue("Text/Misspelled", d->misspelled_color.name());

	// Store spacings
	settings.setValue("Spacings/IndentFirstLine", d->indent_first_line);
	settings.setValue("Spacings/LineSpacing", d->line_spacing.value());
	settings.setValue("Spacings/ParagraphAbove", d->paragraph_spacing_above.value());
	settings.setValue("Spacings/ParagraphBelow", d->paragraph_spacing_below.value());
	settings.setValue("Spacings/TabWidth", d->tab_width.value());
}

//-----------------------------------------------------------------------------
