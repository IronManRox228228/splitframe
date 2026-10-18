#pragma once

// Built-in procedural 512x512 greyscale textures for GPU effects and transitions.

#include <QImage>
#include <QString>
#include <QStringList>

namespace sf::render {

// The 9 supported procedural texture names:
// linear_lr, linear_tb, radial, diamond, spiral, noise, clouds, bars, checker.
const QStringList& proceduralTextureNames();

// Generates a 512x512 RGBA8 greyscale procedural texture. Returns a null image if name is unknown.
QImage createProceduralTexture(const QString& name);

// Returns a cached 512x512 RGBA8 procedural texture (created once on first use).
const QImage& proceduralTexture(const QString& name);

} // namespace sf::render
