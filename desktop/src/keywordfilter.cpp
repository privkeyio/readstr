#include "keywordfilter.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>

#include <re2/re2.h>

#include <string>
#include <string_view>

namespace KeywordFilter {
namespace {
// One rule's expression. RE2 reads the text as UTF-8, finds whether the
// pattern matches anywhere in it without ever backtracking, and folds case
// by Unicode's simple folding when asked to. Nothing captures, which keeps
// it on the fastest matcher; errors are reported here, never logged.
std::shared_ptr<const re2::RE2> compile(const std::string &pattern, bool caseSensitive) {
    re2::RE2::Options options;
    options.set_encoding(re2::RE2::Options::EncodingUTF8);
    options.set_case_sensitive(caseSensitive);
    options.set_never_capture(true);
    options.set_max_mem(regexMaxMemory);
    options.set_log_errors(false);
    return std::make_shared<const re2::RE2>(pattern, options);
}

std::string utf8(const QString &text) {
    const QByteArray bytes = wellFormed(text).toUtf8();
    return std::string(bytes.constData(), size_t(bytes.size()));
}

// Unicode's idea of a word character: a letter, digit, mark, or underscore.
bool isWordCharacter(char32_t c) {
    return c == U'_' || QChar::isLetterOrNumber(c) || QChar::isMark(c);
}

// Whether the code point that ends just before `index` is a word character.
bool wordBefore(const QString &text, qsizetype index) {
    if (index <= 0)
        return false;
    const QChar low = text.at(index - 1);
    if (low.isLowSurrogate() && index >= 2 && text.at(index - 2).isHighSurrogate())
        return isWordCharacter(QChar::surrogateToUcs4(text.at(index - 2), low));
    return isWordCharacter(low.unicode());
}

// Whether the code point that starts at `index` is a word character.
bool wordAt(const QString &text, qsizetype index) {
    if (index >= text.size())
        return false;
    const QChar high = text.at(index);
    if (high.isHighSurrogate() && index + 1 < text.size() && text.at(index + 1).isLowSurrogate())
        return isWordCharacter(QChar::surrogateToUcs4(high, text.at(index + 1)));
    return isWordCharacter(high.unicode());
}

// `word` as a whole word: found where no word character touches it on
// either side, which also finds "C++" or ".NET", where \b around them would
// not. Not with RE2: its \b knows only ASCII word characters, it has no
// lookbehind, and the Unicode classes that would stand in for both make it
// fall back to its slowest matcher, hundreds of times slower than this.
bool containsWord(const QString &text, const QString &word, Qt::CaseSensitivity sensitivity) {
    for (qsizetype from = 0; (from = text.indexOf(word, from, sensitivity)) >= 0; ++from)
        if (!wordBefore(text, from) && !wordAt(text, from + word.size()))
            return true;
    return false;
}

// A pattern RE2 refuses for what it never supports, rather than for a
// mistake: a lookahead or lookbehind, or a reference back to a group.
bool unsupported(const re2::RE2 &expression) {
    const std::string &arg = expression.error_arg();
    const auto startsWith = [&arg](std::string_view prefix) { return arg.compare(0, prefix.size(), prefix) == 0; };
    switch (expression.error_code()) {
    case re2::RE2::ErrorBadPerlOp:
        return startsWith("(?=") || startsWith("(?!") || startsWith("(?<=") || startsWith("(?<!");
    case re2::RE2::ErrorBadEscape:
        return arg.size() >= 2 && ((arg[1] >= '1' && arg[1] <= '9') || arg[1] == 'k' || arg[1] == 'g');
    default: return false;
    }
}

template <typename Enum>
std::optional<Enum> fromName(const QString &name, std::initializer_list<std::pair<const char *, Enum>> names) {
    for (const auto &[text, value] : names)
        if (name == QLatin1String(text))
            return value;
    return std::nullopt;
}

// One field of an article, made, normalized, and encoded only when a rule
// first reads it, and then once for all the rules.
class Subject {
public:
    explicit Subject(std::function<QString()> make) : m_make(std::move(make)) {}

    const QString &text() {
        if (!m_made) {
            m_text = wellFormed(m_make());
            m_made = true;
        }
        return m_text;
    }
    // The first maxRegexSubject bytes of the text, for a regular
    // expression, never ending inside a character.
    std::string_view bytes() {
        if (!m_encoded) {
            const QByteArray encoded = text().toUtf8();
            m_utf8.assign(encoded.constData(), size_t(encoded.size()));
            m_encoded = true;
        }
        size_t size = m_utf8.size();
        if (size > size_t(maxRegexSubject)) {
            size = size_t(maxRegexSubject);
            while (size > 0 && (static_cast<unsigned char>(m_utf8[size]) & 0xC0) == 0x80)
                --size;
        }
        return std::string_view(m_utf8.data(), size);
    }

private:
    std::function<QString()> m_make;
    bool m_made = false;
    bool m_encoded = false;
    QString m_text;
    std::string m_utf8;
};
}

QString wellFormed(const QString &text) {
    QString result;
    const qsizetype size = text.size();
    for (qsizetype i = 0; i < size; ++i) {
        const QChar c = text.at(i);
        if (c.isHighSurrogate() && i + 1 < size && text.at(i + 1).isLowSurrogate()) {
            ++i;
            continue;
        }
        if (!c.isSurrogate())
            continue;
        // Copied only for the rare text that needs it.
        if (result.isNull())
            result = text;
        result[i] = QChar(QChar::ReplacementCharacter);
    }
    return result.isNull() ? text : result;
}

Problem check(Type type, const QString &pattern, QString *detail) {
    if (pattern.isEmpty())
        return Problem::Empty;
    if (pattern.size() > maxPatternLength)
        return Problem::TooLong;
    if (type != Type::Regex)
        return Problem::None;
    // Folding case makes a larger program, so a pattern is checked both
    // ways: one that compiles only minding case would never match without.
    // The web app's heuristic against nested repeats is not applied: RE2
    // matches those in linear time too, and the rules never leave this
    // machine, so it would only refuse patterns that work.
    const std::string source = utf8(pattern);
    for (const bool caseSensitive : {true, false}) {
        const auto expression = compile(source, caseSensitive);
        if (expression->ok())
            continue;
        if (unsupported(*expression))
            return Problem::Unsupported;
        if (detail)
            *detail = expression->error_code() == re2::RE2::ErrorPatternTooLarge
                          ? QStringLiteral("it is too large to match quickly")
                          : QString::fromStdString(expression->error());
        return Problem::Invalid;
    }
    return Problem::None;
}

QString targetName(Target target) {
    switch (target) {
    case Target::Title: return QStringLiteral("title");
    case Target::Content: return QStringLiteral("content");
    case Target::Author: return QStringLiteral("author");
    case Target::Any: break;
    }
    return QStringLiteral("any");
}

QString typeName(Type type) {
    switch (type) {
    case Type::Word: return QStringLiteral("word");
    case Type::Regex: return QStringLiteral("regex");
    case Type::Contains: break;
    }
    return QStringLiteral("contains");
}

QString actionName(Action action) {
    switch (action) {
    case Action::Highlight: return QStringLiteral("highlight");
    case Action::MarkRead: return QStringLiteral("mark-read");
    case Action::Hide: break;
    }
    return QStringLiteral("hide");
}

std::optional<Target> targetFrom(const QString &name) {
    return fromName<Target>(name, {{"title", Target::Title},
                                   {"content", Target::Content},
                                   {"author", Target::Author},
                                   {"any", Target::Any}});
}

std::optional<Type> typeFrom(const QString &name) {
    return fromName<Type>(name, {{"contains", Type::Contains}, {"word", Type::Word}, {"regex", Type::Regex}});
}

std::optional<Action> actionFrom(const QString &name) {
    return fromName<Action>(name,
                            {{"hide", Action::Hide}, {"highlight", Action::Highlight}, {"mark-read", Action::MarkRead}});
}

QJsonObject toJson(const Rule &rule, int order) {
    return {{"id", rule.id},
            {"enabled", rule.enabled},
            {"order", order},
            {"target", targetName(rule.target)},
            {"type", typeName(rule.type)},
            {"pattern", rule.pattern},
            {"caseSensitive", rule.caseSensitive},
            {"action", actionName(rule.action)}};
}

std::optional<Rule> fromJson(const QJsonObject &object, QString *problem) {
    const auto refuse = [problem](const QString &why) {
        if (problem)
            *problem = why;
        return std::nullopt;
    };
    Rule rule;
    rule.pattern = object.value("pattern").toString();
    const auto target = targetFrom(object.value("target").toString());
    const auto type = typeFrom(object.value("type").toString());
    const auto action = actionFrom(object.value("action").toString());
    if (!target)
        return refuse(QStringLiteral("unknown target"));
    if (!type)
        return refuse(QStringLiteral("unknown match type"));
    if (!action)
        return refuse(QStringLiteral("unknown action"));
    switch (check(*type, rule.pattern)) {
    case Problem::None: break;
    case Problem::Empty: return refuse(QStringLiteral("no pattern"));
    case Problem::TooLong: return refuse(QStringLiteral("a pattern longer than %1 characters").arg(maxPatternLength));
    case Problem::Invalid:
    case Problem::Unsupported: return refuse(QStringLiteral("a regular expression that cannot be used"));
    }
    // Absent, each has its default; anything but true or false could be
    // read as a broader rule than the one written.
    const QJsonValue enabled = object.value("enabled"), caseSensitive = object.value("caseSensitive");
    if (!enabled.isUndefined() && !enabled.isBool())
        return refuse(QStringLiteral("\"enabled\" is not true or false"));
    if (!caseSensitive.isUndefined() && !caseSensitive.isBool())
        return refuse(QStringLiteral("\"caseSensitive\" is not true or false"));
    rule.id = object.value("id").toString();
    if (rule.id.isEmpty() || rule.id.size() > 64)
        rule.id = newId();
    rule.enabled = enabled.toBool(true);
    rule.target = *target;
    rule.type = *type;
    rule.caseSensitive = caseSensitive.toBool(false);
    rule.action = *action;
    return rule;
}

QString newId() {
    return QStringLiteral("%1-%2")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(QRandomGenerator::global()->generate64(), 0, 36);
}

QString flagsKey(const QList<Rule> &rules) {
    QJsonArray key;
    for (const Rule &rule : rules)
        if (rule.enabled)
            key << QJsonObject{{"target", targetName(rule.target)},
                               {"type", typeName(rule.type)},
                               {"pattern", rule.pattern},
                               {"caseSensitive", rule.caseSensitive},
                               {"action", actionName(rule.action)}};
    return QString::fromUtf8(QJsonDocument(key).toJson(QJsonDocument::Compact));
}

Rules::Rules(const QList<Rule> &rules) {
    for (const Rule &rule : rules) {
        if (!rule.enabled || check(rule.type, rule.pattern) != Problem::None)
            continue;
        Compiled compiled{rule, {}};
        // The pattern reads as the text does, with any lone surrogate in it
        // replaced, so that it can still match what it was written for.
        compiled.rule.pattern = wellFormed(rule.pattern);
        if (rule.type == Type::Regex)
            compiled.expression = compile(utf8(compiled.rule.pattern), rule.caseSensitive);
        if (compiled.expression && !compiled.expression->ok())
            continue;
        m_needsContent = m_needsContent || rule.target == Target::Content || rule.target == Target::Any;
        m_rules << compiled;
    }
}

Outcome Rules::evaluate(const QString &title, const QString &content, const QString &author) const {
    return evaluateLazily(title, [&content] { return content; }, author);
}

Outcome Rules::evaluateLazily(const QString &title, const std::function<QString()> &content,
                              const QString &author) const {
    Outcome outcome;
    Subject titleSubject([&title] { return title; });
    Subject contentSubject(content);
    Subject authorSubject([&author] { return author; });
    const auto matches = [](const Compiled &compiled, Subject &subject) {
        const Rule &rule = compiled.rule;
        const Qt::CaseSensitivity sensitivity = rule.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
        switch (rule.type) {
        case Type::Contains: return subject.text().contains(rule.pattern, sensitivity);
        case Type::Word: return containsWord(subject.text(), rule.pattern, sensitivity);
        case Type::Regex: return re2::RE2::PartialMatch(subject.bytes(), *compiled.expression);
        }
        return false;
    };
    for (const Compiled &compiled : m_rules) {
        bool matched = false;
        switch (compiled.rule.target) {
        case Target::Title: matched = matches(compiled, titleSubject); break;
        case Target::Content: matched = matches(compiled, contentSubject); break;
        case Target::Author: matched = matches(compiled, authorSubject); break;
        // Each field on its own, as the web app reads them joined by spaces:
        // the same but for a match that would span two fields, and for ^ and
        // $, which here hold at the start and end of each one.
        case Target::Any:
            matched = matches(compiled, titleSubject) || matches(compiled, authorSubject)
                      || matches(compiled, contentSubject);
            break;
        }
        if (!matched)
            continue;
        switch (compiled.rule.action) {
        case Action::Hide: return Outcome{true, false, false};
        case Action::MarkRead: outcome.markRead = true; break;
        case Action::Highlight: outcome.highlight = true; break;
        }
    }
    return outcome;
}
}
