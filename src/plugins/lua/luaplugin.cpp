/*
 * Lua Tiled Plugin
 * Copyright 2011, Thorbjørn Lindeijer <thorbjorn@lindeijer.nl>
 *
 * This file is part of Tiled.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "luaplugin.h"

#include "luatablewriter.h"

#include "imagelayer.h"
#include "map.h"
#include "mapobject.h"
#include "objectgroup.h"
#include "properties.h"
#include "terrain.h"
#include "tile.h"
#include "tilelayer.h"
#include "tileset.h"

#include <QFile>
#include <QCoreApplication>

#if QT_VERSION >= 0x050100
#define HAS_QSAVEFILE_SUPPORT
#endif

#ifdef HAS_QSAVEFILE_SUPPORT
#include <QSaveFile>
#endif

/**
 * See below for an explanation of the different formats. One of these needs
 * to be defined.
 */
//#define POLYGON_FORMAT_FULL
//#define POLYGON_FORMAT_PAIRS
//#define POLYGON_FORMAT_OPTIMAL

// MOAI friendly
#define POLYGON_FORMAT_SEQUENCE
#define MOAI_LUA_DATA_FORMAT
#define FIRST_IMAGE
#define FLATTEN_INTO_PARENT
//#define ALL_IMAGES


using namespace Lua;
using namespace Tiled;

LuaPlugin::LuaPlugin()
{
}

bool LuaPlugin::write(const Map *map, const QString &fileName)
{
#ifdef HAS_QSAVEFILE_SUPPORT
    QSaveFile file(fileName);
#else
    QFile file(fileName);
#endif
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        mError = tr("Could not open file for writing.");
        return false;
    }

    mMapDir = QFileInfo(fileName).path();

    LuaTableWriter writer(&file);
    writer.writeStartDocument();
    writeMap(writer, map);
    writer.writeEndDocument();

    if (file.error() != QFile::NoError) {
        mError = file.errorString();
        return false;
    }

#ifdef HAS_QSAVEFILE_SUPPORT
    if (!file.commit()) {
        mError = file.errorString();
        return false;
    }
#endif

    return true;
}

QString LuaPlugin::nameFilter() const
{
    return tr("Lua files (*.lua)");
}

QString LuaPlugin::errorString() const
{
    return mError;
}

void LuaPlugin::writeMap(LuaTableWriter &writer, const Map *map)
{
    writer.writeStartReturnTable();

    writer.writeKeyAndValue("version", "1.1");
    writer.writeKeyAndValue("luaversion", "5.1");
    writer.writeKeyAndValue("tiledversion", QCoreApplication::applicationVersion());

    const QString orientation = orientationToString(map->orientation());

    writer.writeKeyAndValue("orientation", orientation);
    writer.writeKeyAndValue("width", map->width());
    writer.writeKeyAndValue("height", map->height());
#if defined(MOAI_LUA_DATA_FORMAT)
    writer.writeKeyAndValue("cellwidth", map->tileWidth());
    writer.writeKeyAndValue("cellheight", map->tileHeight());
#else
    writer.writeKeyAndValue("tilewidth", map->tileWidth());
    writer.writeKeyAndValue("tileheight", map->tileHeight());
#endif
    writer.writeKeyAndValue("nextobjectid", map->nextObjectId());

    if (map->orientation() == Map::Hexagonal)
        writer.writeKeyAndValue("hexsidelength", map->hexSideLength());

    if (map->orientation() == Map::Staggered || map->orientation() == Map::Hexagonal) {
        writer.writeKeyAndValue("staggeraxis",
                                staggerAxisToString(map->staggerAxis()));
        writer.writeKeyAndValue("staggerindex",
                                staggerIndexToString(map->staggerIndex()));
    }

    const QColor &backgroundColor = map->backgroundColor();
    if (backgroundColor.isValid()) {
        // Example: backgroundcolor = { 255, 200, 100 }
        writer.writeStartTable("backgroundcolor");
        writer.setSuppressNewlines(true);
        writer.writeValue(backgroundColor.red());
        writer.writeValue(backgroundColor.green());
        writer.writeValue(backgroundColor.blue());
        if (backgroundColor.alpha() != 255)
            writer.writeValue(backgroundColor.alpha());
        writer.writeEndTable();
        writer.setSuppressNewlines(false);
    }

    writeProperties(writer, map->properties());

    writer.writeStartTable("tilesets");

    mGidMapper.clear();
    unsigned firstGid = 1;
    foreach (Tileset *tileset, map->tilesets()) {
        writeTileset(writer, tileset, firstGid);
        mGidMapper.insert(firstGid, tileset);
        firstGid += tileset->tileCount();
    }
    writer.writeEndTable();

    unsigned short prio = 1;
    writer.writeStartTable("layers");
    foreach (const Layer *layer, map->layers()) {
        switch (layer->layerType()) {
        case Layer::TileLayerType:
            writeTileLayer(prio, writer, static_cast<const TileLayer*>(layer));
            break;
        case Layer::ObjectGroupType:
            writeObjectGroup(prio, writer, static_cast<const ObjectGroup*>(layer));
            break;
        case Layer::ImageLayerType:
            writeImageLayer(prio, writer, static_cast<const ImageLayer*>(layer));
            break;
        }
        prio++;
    }
    writer.writeEndTable();

    writer.writeEndTable();
}

void LuaPlugin::writeProperties(LuaTableWriter &writer,
                                const Properties &properties)
{
    writer.writeStartTable("properties");

    Properties::const_iterator it = properties.constBegin();
    Properties::const_iterator it_end = properties.constEnd();
    for (; it != it_end; ++it)
        writer.writeQuotedKeyAndValue(it.key(), it.value());

    writer.writeEndTable();
}

static bool includeTile(const Tile *tile)
{
    if (!tile->properties().isEmpty())
        return true;
    if (!tile->imageSource().isEmpty())
        return true;
    if (tile->objectGroup())
        return true;
    if (tile->isAnimated())
        return true;
    if (tile->terrain() != 0xFFFFFFFF)
        return true;
    if (tile->terrainProbability() != -1.f)
        return true;

    return false;
}

void LuaPlugin::writeTileset(LuaTableWriter &writer, const Tileset *tileset,
                             unsigned firstGid)
{
#if defined(MOAI_LUA_DATA_FORMAT)
    if (!tileset->imageSource().isEmpty()) {
        const QString image = mMapDir.relativeFilePath(tileset->imageSource()).split("/").takeLast();
        writer.writeQuotedStartTable(image.toLatin1());
    } else {
        writer.writeStartTable();
    }
#else
    writer.writeStartTable();
#endif
    writer.writeKeyAndValue("name", tileset->name());
    
    #if !defined(MOAI_LUA_DATA_FORMAT)
        writer.writeKeyAndValue("firstgid", firstGid);
    #endif

    if (!tileset->fileName().isEmpty()) {
        const QString rel = mMapDir.relativeFilePath(tileset->fileName());
        writer.writeKeyAndValue("filename", rel);
    }

    /* Include all tileset information even for external tilesets, since the
     * external reference is generally a .tsx file (in XML format).
     */
    writer.writeKeyAndValue("tilewidth", tileset->tileWidth());
    writer.writeKeyAndValue("tileheight", tileset->tileHeight());
    writer.writeKeyAndValue("spacing", tileset->tileSpacing());
    writer.writeKeyAndValue("margin", tileset->margin());

    if (!tileset->imageSource().isEmpty()) {
        #if !defined(MOAI_LUA_DATA_FORMAT)
            const QString rel = mMapDir.relativeFilePath(tileset->imageSource());
            writer.writeKeyAndValue("image", rel);
        #endif
        writer.writeKeyAndValue("imagewidth", tileset->imageWidth());
        writer.writeKeyAndValue("imageheight", tileset->imageHeight());
        #if defined(MOAI_LUA_DATA_FORMAT)
            writer.writeKeyAndValue("deckwidth", tileset->imageWidth() / tileset->tileWidth());
            writer.writeKeyAndValue("deckheight", tileset->imageHeight() / tileset->tileHeight());
        #endif
    }

    if (tileset->transparentColor().isValid()) {
        writer.writeKeyAndValue("transparentcolor",
                                tileset->transparentColor().name());
    }

    const QPoint offset = tileset->tileOffset();
#if defined(FLATTEN_INTO_PARENT)
    writer.writeKeyAndValue("xoffset", offset.x());
    writer.writeKeyAndValue("yoffset", offset.y());
#else
    writer.writeStartTable("tileoffset");
    writer.writeKeyAndValue("x", offset.x());
    writer.writeKeyAndValue("y", offset.y());
    writer.writeEndTable();
#endif

    writeProperties(writer, tileset->properties());

    writer.writeStartTable("terrains");
    for (int i = 0; i < tileset->terrainCount(); ++i) {
        const Terrain *t = tileset->terrain(i);
        writer.writeStartTable();

        writer.writeKeyAndValue("name", t->name());
        writer.writeKeyAndValue("tile", t->imageTileId());

        writeProperties(writer, t->properties());

        writer.writeEndTable();
    }
    writer.writeEndTable();

    writer.writeStartTable("tiles");
    for (int i = 0; i < tileset->tileCount(); ++i) {
        const Tile *tile = tileset->tileAt(i);

        // For brevity only write tiles with interesting properties
        if (!includeTile(tile))
            continue;

#if defined(MOAI_LUA_DATA_FORMAT)
        writer.writeQuotedStartTable(QString("id = %1").arg(i + 1).toLatin1());
#else
        writer.writeStartTable();
        writer.writeKeyAndValue("id", i);    
#endif

        if (!tile->properties().isEmpty())
            writeProperties(writer, tile->properties());

        if (!tile->imageSource().isEmpty()) {
            const QString src = mMapDir.relativeFilePath(tile->imageSource());
            const QSize tileSize = tile->size();
            writer.writeKeyAndValue("image", src);
            if (!tileSize.isNull()) {
                writer.writeKeyAndValue("width", tileSize.width());
                writer.writeKeyAndValue("height", tileSize.height());
            }
        }

        unsigned terrain = tile->terrain();
        if (terrain != 0xFFFFFFFF) {
            writer.writeStartTable("terrain");
            writer.setSuppressNewlines(true);
            for (int i = 0; i < 4; ++i )
                writer.writeValue(tile->cornerTerrainId(i));
            writer.writeEndTable();
            writer.setSuppressNewlines(false);
        }

        if (tile->terrainProbability() != -1.f)
            writer.writeKeyAndValue("probability", tile->terrainProbability());

        if (ObjectGroup *objectGroup = tile->objectGroup())
            writeObjectGroup(0, writer, objectGroup, "objectGroup");

        if (tile->isAnimated()) {
            const QVector<Frame> &frames = tile->frames();

            writer.writeStartTable("animation");
            foreach (const Frame &frame, frames) {
                writer.writeStartTable();
                writer.writeKeyAndValue("tileid", QString::number(frame.tileId));
                writer.writeKeyAndValue("duration", QString::number(frame.duration));
                writer.writeEndTable();
            }
            writer.writeEndTable(); // animation
        }

        writer.writeEndTable(); // tile
    }
    writer.writeEndTable(); // tiles

    writer.writeEndTable(); // tileset
}
#if defined(MOAI_LUA_DATA_FORMAT)
void LuaPlugin::writeTileLayer(unsigned short prio,
                               LuaTableWriter &writer,
                               const TileLayer *tileLayer)
{
    writer.writeQuotedStartTable(tileLayer->name().toLatin1());
    writer.writeKeyAndValue("prio", prio);
#else
void LuaPlugin::writeTileLayer(LuaTableWriter &writer,
                               const TileLayer *tileLayer)
{
    writer.writeStartTable();
    writer.writeKeyAndValue("name", tileLayer->name());
#endif

    writer.writeKeyAndValue("type", "tilelayer");
    #if defined(MOAI_LUA_DATA_FORMAT)
        #if defined(FIRST_IMAGE)
            const QList<Tileset *> usedTilelist = tileLayer->usedTilesets().values();
            if (!usedTilelist.isEmpty() && usedTilelist.size() == 1) {
                const QString image = mMapDir.relativeFilePath(usedTilelist.at(0)->imageSource()).split("/").takeLast();
                writer.writeKeyAndValue("image", image);
            }
        #endif
        #if defined(ALL_IMAGES)
            writer.writeStartTable("images");
            const QSet<Tileset *> usedTilesets = tileLayer->usedTilesets();
            if (!usedTilesets.empty())
                foreach (const Tileset * tileset, usedTilesets) {
                    const QString fileId = mMapDir.relativeFilePath(tileset->imageSource()).split("/").takeLast();
                    writer.writeValue(fileId);
                }
            writer.writeEndTable();
        #endif
    #endif
    writer.writeKeyAndValue("x", tileLayer->x());
    writer.writeKeyAndValue("y", tileLayer->y());
    writer.writeKeyAndValue("width", tileLayer->width());
    writer.writeKeyAndValue("height", tileLayer->height());
    writer.writeKeyAndValue("visible", tileLayer->isVisible());
    writer.writeKeyAndValue("opacity", tileLayer->opacity());
    writeProperties(writer, tileLayer->properties());

    writer.writeKeyAndValue("encoding", "lua");

#if defined(MOAI_LUA_DATA_FORMAT)
    QSet<Tileset*> usedTilesets = tileLayer->usedTilesets();
    bool authorizeWriteTable = true;
    for (int y = 0; y < tileLayer->height(); ++y) {
         for (int x = 0; x < tileLayer->width(); ++x) {
            foreach (Tileset *tileset, usedTilesets) {
                unsigned tileId = mGidMapper.cellToGidOrigin(tileLayer->cellAt(x, y));
                if(tileId > 0 && !tileset->tileAt(tileId - 1)->properties().isEmpty()) {
                    if(authorizeWriteTable) {
                       writer.writeStartTable("specialtiles");
                       authorizeWriteTable = false;
                    }  // keys are ordered alphabetically!
                    writer.writeQuotedStartTable(QString("y = %1, x = %2").arg(y + 1).arg(x + 1).toLatin1());
                    writer.writeKeyAndValue("id", tileId);
                    writer.writeEndTable();
                    break;
                }
            }
         }
    }
    if(!authorizeWriteTable) {
        writer.writeEndTable();
    }
#endif

    writer.writeStartTable("data");
    for (int y = 0; y < tileLayer->height(); ++y) {
        #if defined(MOAI_LUA_DATA_FORMAT)
            writer.prepareNewLine();
            writer.setSuppressNewlines(true);
            writer.writeStartTable();
            writer.writeValue(y + 1);
        #else
            if (y > 0) {
                writer.prepareNewLine();
            }
        #endif
        for (int x = 0; x < tileLayer->width(); ++x) {
            #if defined(MOAI_LUA_DATA_FORMAT)
                writer.writeValue(mGidMapper.cellToGidOrigin(tileLayer->cellAt(x, y)));
            #else
                writer.writeValue(mGidMapper.cellToGid(tileLayer->cellAt(x, y)));
            #endif
        }
        #if defined(MOAI_LUA_DATA_FORMAT)
            writer.writeEndTable();
            writer.setSuppressNewlines(false);
        #endif
    }
    writer.writeEndTable();
    writer.writeEndTable();
}

#if defined(MOAI_LUA_DATA_FORMAT)
void LuaPlugin::writeObjectGroup(unsigned short prio,
                                 LuaTableWriter &writer,
                                 const ObjectGroup *objectGroup,
                                 const QByteArray &key)
{
    writer.writeQuotedStartTable(objectGroup->name().toLatin1());
    if (! (prio == 0)) {
        writer.writeKeyAndValue("prio", prio);
    }
#else
void LuaPlugin::writeObjectGroup(LuaTableWriter &writer,
                                 const ObjectGroup *objectGroup,
                                 const QByteArray &key)
{
    if (key.isEmpty())
        writer.writeStartTable();
    else
        writer.writeStartTable(key);
    writer.writeKeyAndValue("name", objectGroup->name());
#endif
    writer.writeKeyAndValue("type", "objectgroup");
    writer.writeKeyAndValue("visible", objectGroup->isVisible());
    writer.writeKeyAndValue("opacity", objectGroup->opacity());
    writeProperties(writer, objectGroup->properties());

    writer.writeStartTable("objects");
    foreach (MapObject *mapObject, objectGroup->objects())
        writeMapObject(writer, mapObject);
    writer.writeEndTable();

    writer.writeEndTable();
}

#if defined(MOAI_LUA_DATA_FORMAT)
void LuaPlugin::writeImageLayer(unsigned short prio,
                                LuaTableWriter &writer,
                                const ImageLayer *imageLayer)
{
    writer.writeQuotedStartTable(imageLayer->name().toLatin1());
    writer.writeKeyAndValue("prio", prio);
#elif
void LuaPlugin::writeImageLayer(LuaTableWriter &writer,
                                const ImageLayer *imageLayer)
{
    writer.writeStartTable();
    writer.writeKeyAndValue("name", imageLayer->name());
#endif

    writer.writeKeyAndValue("type", "imagelayer");
    writer.writeKeyAndValue("x", imageLayer->x());
    writer.writeKeyAndValue("y", imageLayer->y());
    writer.writeKeyAndValue("visible", imageLayer->isVisible());
    writer.writeKeyAndValue("opacity", imageLayer->opacity());

    const QString rel = mMapDir.relativeFilePath(imageLayer->imageSource());
    writer.writeKeyAndValue("image", rel);

    if (imageLayer->transparentColor().isValid()) {
        writer.writeKeyAndValue("transparentcolor",
                                imageLayer->transparentColor().name());
    }

    writeProperties(writer, imageLayer->properties());

    writer.writeEndTable();
}

static const char *toString(MapObject::Shape shape)
{
    switch (shape) {
    case MapObject::Rectangle:
        return "rectangle";
    case MapObject::Polygon:
        return "polygon";
    case MapObject::Polyline:
        return "polyline";
    case MapObject::Ellipse:
        return "ellipse";
    }
    return "unknown";
}

void LuaPlugin::writeMapObject(LuaTableWriter &writer,
                               const Tiled::MapObject *mapObject)
{
    writer.writeStartTable();
#if !defined(MOAI_LUA_DATA_FORMAT)
    writer.writeKeyAndValue("id", mapObject->id());
#endif
    writer.writeKeyAndValue("name", mapObject->name());
    writer.writeKeyAndValue("type", mapObject->type());
    writer.writeKeyAndValue("shape", toString(mapObject->shape()));

    writer.writeKeyAndValue("x", mapObject->x());
    writer.writeKeyAndValue("y", mapObject->y());
    writer.writeKeyAndValue("width", mapObject->width());
    writer.writeKeyAndValue("height", mapObject->height());
    writer.writeKeyAndValue("rotation", mapObject->rotation());

    if (!mapObject->cell().isEmpty())
        writer.writeKeyAndValue("gid", mGidMapper.cellToGid(mapObject->cell()));

    writer.writeKeyAndValue("visible", mapObject->isVisible());

    const QPolygonF &polygon = mapObject->polygon();
    if (!polygon.isEmpty()) {
        if (mapObject->shape() == MapObject::Polygon)
            writer.writeStartTable("polygon");
        else
            writer.writeStartTable("polyline");

#if defined(POLYGON_FORMAT_FULL)
        /* This format is the easiest to read and understand:
         *
         *  {
         *    { x = 1, y = 1 },
         *    { x = 2, y = 2 },
         *    { x = 3, y = 3 },
         *    ...
         *  }
         */
        foreach (const QPointF &point, polygon) {
            writer.writeStartTable();
            writer.setSuppressNewlines(true);

            writer.writeKeyAndValue("x", point.x());
            writer.writeKeyAndValue("y", point.y());

            writer.writeEndTable();
            writer.setSuppressNewlines(false);
        }
#elif defined(POLYGON_FORMAT_PAIRS)
        /* This is an alternative that takes about 25% less memory.
         *
         *  {
         *    { 1, 1 },
         *    { 2, 2 },
         *    { 3, 3 },
         *    ...
         *  }
         */
        foreach (const QPointF &point, polygon) {
            writer.writeStartTable();
            writer.setSuppressNewlines(true);

            writer.writeValue(point.x());
            writer.writeValue(point.y());

            writer.writeEndTable();
            writer.setSuppressNewlines(false);
        }
#elif defined(POLYGON_FORMAT_OPTIMAL)
        /* Writing it out in two tables, one for the x coordinates and one for
         * the y coordinates. It is a compromise between code readability and
         * performance. This takes the least amount of memory (60% less than
         * the first approach).
         *
         * x = { 1, 2, 3, ... }
         * y = { 1, 2, 3, ... }
         */

        writer.writeStartTable("x");
        writer.setSuppressNewlines(true);
        foreach (const QPointF &point, polygon)
            writer.writeValue(point.x());
        writer.writeEndTable();
        writer.setSuppressNewlines(false);

        writer.writeStartTable("y");
        writer.setSuppressNewlines(true);
        foreach (const QPointF &point, polygon)
            writer.writeValue(point.y());
        writer.writeEndTable();
        writer.setSuppressNewlines(false);
#elif defined(POLYGON_FORMAT_SEQUENCE)
        /* Writing it out in sequence like this: {x0, y0, x1, y1...}.
         * { 2, 0, 1, 6... }
         */
        writer.setSuppressNewlines(true);
        foreach (const QPointF &point, polygon) {
            writer.writeValue(unsigned int(point.x()));
            writer.writeValue(unsigned int(point.y()));
        }
#endif

        writer.writeEndTable();
        #if defined(POLYGON_FORMAT_SEQUENCE)
            writer.setSuppressNewlines(false);
        #endif
    }

    writeProperties(writer, mapObject->properties());

    writer.writeEndTable();
}

#if QT_VERSION < 0x050000
Q_EXPORT_PLUGIN2(Lua, LuaPlugin)
#endif
