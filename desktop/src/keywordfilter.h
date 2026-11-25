#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

namespace re2 {
class RE2;
}

// Keyword filter rules, as the web app has them (src/lib/keyword-filter.ts):
// each matches a word, a phrase, or a regular expression in an article's
// title, text, author, or any of them, and hides it, highlights it, or marks
// it read when it arrives. Rules stay on this machine; nothing here is ever
// published. No GUI types, so the Store can apply them on any thread.
namespace KeywordFilter {
enum class Target { Title, Content, Author, Any };
enum class Type { Contains, Word, Regex };
enum class Action { Hide, Highlight, MarkRead };

struct Rule {
    QString id;
    bool enabled = true;
    Target target = Target::Any;
    Type type = Type::Contains;
    QString pattern;
    bool caseSensitive = false;
    Action action = Action::Hide;

    bool operator==(const Rule &other) const {
        return id == other.id && enabled == other.enabled && target == other.target && type == other.type
               && pattern == other.pattern && caseSensitive == other.caseSensitive && action == other.action;
    }
};

constexpr int maxRules = 200;
constexpr int maxPatternLength = 200;
// Regular expressions are matched with RE2, whose time
// grows linearly with the text instead of with the backtracking a pattern
// can force. RE2 refuses a pattern whose compiled form would take more
// memory than this, and keeps the cache it matches with within it, so 200
// rules stay within 200 MiB at the very worst; real patterns take a few KiB.
constexpr qint64 regexMaxMemory = 1024 * 1024;
// RE2's time also grows with the size of the compiled pattern when its cache
// is not enough and it steps through every state at once: the largest
// patterns it accepts take about half a second for this much text. So a
// regular expression reads at most the first 64 KiB of each field (in UTF-8,
// cut at a character's boundary), where the text a reader sees first is. A
// whole word and plain text are looked for everywhere, in time that grows
// with the text and the pattern's length only.
constexpr int maxRegexSubject = 64 * 1024;

// Why a pattern cannot be used: a regular expression RE2 cannot compile is
// Invalid, or Unsupported when it uses a lookaround or a backreference,
// which RE2 never has because they are what lets a pattern backtrack
// without end.
enum class Problem { None, Empty, TooLong, Invalid, Unsupported };
// `detail` gets RE2's reason for an Invalid pattern.
Problem check(Type type, const QString &pattern, QString *detail = nullptr);

QString targetName(Target target);
QString typeName(Type type);
QString actionName(Action action);
std::optional<Target> targetFrom(const QString &name);
std::optional<Type> typeFrom(const QString &name);
std::optional<Action> actionFrom(const QString &name);

QJsonObject toJson(const Rule &rule, int order);
// The rule `object` describes, or null with the reason in `problem` when it
// is not one this version would save: no pattern, one past
// maxPatternLength (cutting it would change what a regular expression
// matches), one check() refuses, or an unknown target, type, or action
// (guessing one could hide what the user never asked to hide).
std::optional<Rule> fromJson(const QJsonObject &object, QString *problem = nullptr);
QString newId();
// What decides the flags `rules` give an article: the enabled ones, in
// order, without their ids. Two lists with the same key hide, highlight,
// and mark read the same articles.
QString flagsKey(const QList<Rule> &rules);
// `text` with each UTF-16 surrogate that is not half of a pair replaced by
// U+FFFD, as SQLite stores it, so an article reads the same to the rules
// when it arrives and when it is read back from the library.
QString wellFormed(const QString &text);

// What the rules make of one article. A hide wins over everything and ends
// the evaluation; mark-read and highlight add up.
struct Outcome {
    bool hidden = false;
    bool highlight = false;
    bool markRead = false;

    // As the library keeps it (items.filter_flags).
    enum Flag { Hidden = 1, Highlighted = 2, MarkedRead = 4 };
    int flags() const { return (hidden ? Hidden : 0) | (highlight ? Highlighted : 0) | (markRead ? MarkedRead : 0); }
};

// The enabled rules, in order, each compiled once.
class Rules {
public:
    Rules() = default;
    explicit Rules(const QList<Rule> &rules);

    bool isEmpty() const { return m_rules.isEmpty(); }
    // Whether some rule reads the article's text, which is costly to make.
    bool needsContent() const { return m_needsContent; }
    // A rule for "any" matches when the title, the text, or the author
    // does, each read on its own. `content` is the article's plain text.
    Outcome evaluate(const QString &title, const QString &content, const QString &author) const;
    // The same, with the text made by `content` only once a rule reaches
    // it, and at most once.
    Outcome evaluateLazily(const QString &title, const std::function<QString()> &content,
                           const QString &author) const;

private:
    struct Compiled {
        Rule rule;
        // Only for Regex.
        std::shared_ptr<const re2::RE2> expression;
    };
    QList<Compiled> m_rules;
    bool m_needsContent = false;
};
}
