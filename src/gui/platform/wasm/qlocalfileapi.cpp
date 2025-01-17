// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qlocalfileapi_p.h"
#include <private/qstdweb_p.h>
#include <QtCore/QRegularExpression>

QT_BEGIN_NAMESPACE
namespace LocalFileApi {
namespace {
std::optional<emscripten::val> qtFilterListToTypes(const QStringList &filterList)
{
    using namespace qstdweb;
    using namespace emscripten;

    auto types = emscripten::val::array();

    for (const auto &fileFilter : filterList) {
        auto type = Type::fromQt(fileFilter);
        if (type)
            types.call<void>("push", type->asVal());
    }

    return types["length"].as<int>() == 0 ? std::optional<emscripten::val>() : types;
}
}

Type::Type(QStringView description, std::optional<Accept> accept)
    : m_storage(emscripten::val::object())
{
    m_storage.set("description", description.trimmed().toString().toStdString());
    if (accept)
        m_storage.set("accept", accept->asVal());
}

Type::~Type() = default;

std::optional<Type> Type::fromQt(QStringView type)
{
    using namespace emscripten;

    // Accepts either a string in format:
    // GROUP3
    // or in this format:
    // GROUP1 (GROUP2)
    // Group 1 is treated as the description, whereas group 2 or 3 are treated as the filter list.
    static QRegularExpression regex(
            QString(QStringLiteral("(?:(?:([^(]*)\\(([^()]+)\\)[^)]*)|([^()]+))")));
    const auto match = regex.match(type);

    if (!match.hasMatch())
        return std::nullopt;

    constexpr size_t DescriptionIndex = 1;
    constexpr size_t FilterListFromParensIndex = 2;
    constexpr size_t PlainFilterListIndex = 3;

    const auto description = match.hasCaptured(DescriptionIndex)
            ? match.capturedView(DescriptionIndex)
            : QStringView();
    const auto filterList = match.capturedView(match.hasCaptured(FilterListFromParensIndex)
                                                       ? FilterListFromParensIndex
                                                       : PlainFilterListIndex);

    auto accept = Type::Accept::fromQt(filterList);
    if (!accept)
        return std::nullopt;

    return Type(description, std::move(*accept));
}

emscripten::val Type::asVal() const
{
    return m_storage;
}

Type::Accept::Accept() : m_storage(emscripten::val::object()) { }

Type::Accept::~Accept() = default;

std::optional<Type::Accept> Type::Accept::fromQt(QStringView qtRepresentation)
{
    Accept accept;

    // Used for accepting multiple extension specifications on a filter list.
    // The next group of non-empty characters.
    static QRegularExpression internalRegex(QString(QStringLiteral("([^\\s]+)\\s*")));
    int offset = 0;
    auto internalMatch = internalRegex.match(qtRepresentation, offset);
    MimeType mimeType;

    while (internalMatch.hasMatch()) {
        auto webExtension = MimeType::Extension::fromQt(internalMatch.capturedView(1));

        if (!webExtension)
            return std::nullopt;

        mimeType.addExtension(*webExtension);

        internalMatch = internalRegex.match(qtRepresentation, internalMatch.capturedEnd());
    }

    accept.addMimeType(mimeType);
    return accept;
}

void Type::Accept::addMimeType(MimeType mimeType)
{
    // The mime type provided here does not seem to have any effect at the result at all.
    m_storage.set("application/octet-stream", mimeType.asVal());
}

emscripten::val Type::Accept::asVal() const
{
    return m_storage;
}

Type::Accept::MimeType::MimeType() : m_storage(emscripten::val::array()) { }

Type::Accept::MimeType::~MimeType() = default;

void Type::Accept::MimeType::addExtension(Extension extension)
{
    m_storage.call<void>("push", extension.asVal());
}

emscripten::val Type::Accept::MimeType::asVal() const
{
    return m_storage;
}

Type::Accept::MimeType::Extension::Extension(QStringView extension)
    : m_storage(extension.toString().toStdString())
{
}

Type::Accept::MimeType::Extension::~Extension() = default;

std::optional<Type::Accept::MimeType::Extension>
Type::Accept::MimeType::Extension::fromQt(QStringView qtRepresentation)
{
    // Checks for a filter that matches everything:
    // Any number of asterisks or any number of asterisks with a '.' between them.
    // The web filter does not support wildcards.
    static QRegularExpression qtAcceptAllRegex(
            QRegularExpression::anchoredPattern(QString(QStringLiteral("[*]+|[*]+\\.[*]+"))));
    if (qtAcceptAllRegex.match(qtRepresentation).hasMatch())
        return std::nullopt;

    // Checks for correctness. The web filter only allows filename extensions and does not filter
    // the actual filenames, therefore we check whether the filter provided only filters for the
    // extension.
    static QRegularExpression qtFilenameMatcherRegex(
            QRegularExpression::anchoredPattern(QString(QStringLiteral("(\\*?)(\\.[^*]+)"))));

    auto extensionMatch = qtFilenameMatcherRegex.match(qtRepresentation);
    if (extensionMatch.hasMatch())
        return Extension(extensionMatch.capturedView(2));

    // Mapping impossible.
    return std::nullopt;
}

emscripten::val Type::Accept::MimeType::Extension::asVal() const
{
    return m_storage;
}

emscripten::val makeOpenFileOptions(const QStringList &filterList, bool acceptMultiple)
{
    auto options = emscripten::val::object();
    if (auto typeList = LocalFileApi::qtFilterListToTypes(filterList)) {
        options.set("types", std::move(*typeList));
        options.set("excludeAcceptAllOption", true);
    }

    options.set("multiple", acceptMultiple);

    return options;
}

emscripten::val makeSaveFileOptions(const QStringList &filterList, const std::string& suggestedName)
{
    auto options = emscripten::val::object();

    if (!suggestedName.empty())
        options.set("suggestedName", emscripten::val(suggestedName));

    if (auto typeList = LocalFileApi::qtFilterListToTypes(filterList))
        options.set("types", emscripten::val(std::move(*typeList)));

    return options;
}

}  // namespace LocalFileApi

QT_END_NAMESPACE
