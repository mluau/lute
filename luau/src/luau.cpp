#include "lute/luau.h"

#include "Luau/Ast.h"
#include "Luau/Location.h"
#include "Luau/ParseResult.h"
#include "Luau/Parser.h"
#include "Luau/ParseOptions.h"
#include "Luau/ToString.h"
#include "Luau/Compiler.h"

#include "lute/userdatas.h"


#include "lua.h"
#include "lualib.h"
#include <cstddef>
#include <cstring>
#include <iterator>

const char* COMPILE_RESULT_TYPE = "CompileResult";

LUAU_FASTFLAG(LuauStoreCSTData2)
LUAU_FASTFLAG(LuauParseOptionalAsNode2)

namespace luau
{

struct StatResult
{
    std::shared_ptr<Luau::Allocator> allocator;
    std::shared_ptr<Luau::AstNameTable> names;

    Luau::ParseResult parseResult;
};

static StatResult parse(std::string& source)
{
    // TODO: this is very bad, fix it!
    FFlag::LuauStoreCSTData2.value = true;
    FFlag::LuauParseOptionalAsNode2.value = true;

    auto allocator = std::make_shared<Luau::Allocator>();
    auto names = std::make_shared<Luau::AstNameTable>(*allocator);

    Luau::ParseOptions options;
    options.captureComments = true;
    options.allowDeclarationSyntax = false;
    options.storeCstData = true;

    auto parseResult = Luau::Parser::parse(source.data(), source.size(), *names, *allocator, options);

    return StatResult{allocator, names, std::move(parseResult)};
}

struct ExprResult
{
    std::shared_ptr<Luau::Allocator> allocator;
    std::shared_ptr<Luau::AstNameTable> names;

    Luau::ParseExprResult parseResult;
};

static ExprResult parseExpr(std::string& source)
{
    // TODO: this is very bad, fix it!
    FFlag::LuauStoreCSTData2.value = true;
    FFlag::LuauParseOptionalAsNode2.value = true;

    auto allocator = std::make_shared<Luau::Allocator>();
    auto names = std::make_shared<Luau::AstNameTable>(*allocator);

    Luau::ParseOptions options;
    options.captureComments = true;
    options.allowDeclarationSyntax = false;
    options.storeCstData = true;

    auto parseResult = Luau::Parser::parseExpr(source.data(), source.size(), *names, *allocator, options);

    return ExprResult{allocator, names, std::move(parseResult)};
}

static std::vector<size_t> computeLineOffsets(std::string_view content)
{
    std::vector<size_t> result{};
    result.emplace_back(0);

    for (size_t i = 0; i < content.size(); i++)
    {
        auto ch = content[i];
        if (ch == '\r' || ch == '\n')
        {
            if (ch == '\r' && i + 1 < content.size() && content[i + 1] == '\n')
            {
                i++;
            }
            result.push_back(i + 1);
        }
    }
    return result;
}

static std::vector<Luau::Comment> commentsWithinSpan(const std::vector<Luau::Comment> comments, Luau::Location span)
{
    // TODO: O(n), we could probably binary search if there are a lot of comments
    std::vector<Luau::Comment> result;

    for (const auto& comment : comments)
        if (span.encloses(comment.location))
            result.emplace_back(comment);

    return result;
}

struct Trivia
{
    enum TriviaKind
    {
        Whitespace,
        SingleLineComment,
        MultiLineComment,
    };

    TriviaKind kind;
    Luau::Location location;
    std::string_view text;
};

struct AstSerialize : public Luau::AstVisitor
{
    lua_State* L;
    Luau::CstNodeMap cstNodeMap;
    std::string_view source;
    Luau::Position currentPosition{0, 0};
    std::vector<size_t> lineOffsets;
    std::vector<Luau::Comment> commentLocations;

    // absolute index for the table where we're storing locals
    int localTableIndex;
    // reference to previously serialized token
    int lastTokenRef = LUA_NOREF;

    AstSerialize(lua_State* L, std::string_view source, Luau::CstNodeMap cstNodeMap, std::vector<Luau::Comment> commentLocations)
        : L(L)
        , cstNodeMap(std::move(cstNodeMap))
        , source(source)
        , lineOffsets(computeLineOffsets(source))
        , commentLocations(std::move(commentLocations))
    {
        lua_createtable(L, 0, 0);
        localTableIndex = lua_absindex(L, -1);
    }

    template<typename T>
    T* lookupCstNode(Luau::AstNode* astNode)
    {
        // TODO: use find instead
        if (const auto cstNode = cstNodeMap[astNode])
            return cstNode->as<T>();
        return nullptr;
    }

    void advancePosition(std::string_view contents)
    {
        if (contents.empty())
            return;

        size_t index = 0;
        size_t numLines = 0;
        while (true)
        {
            auto newlinePos = contents.find('\n', index);
            if (newlinePos == std::string::npos)
                break;
            numLines++;
            index = newlinePos + 1;
        }

        currentPosition.line += numLines;
        if (numLines > 0)
            currentPosition.column = unsigned(contents.size()) - index;
        else
            currentPosition.column += unsigned(contents.size());
    }

    std::vector<Trivia> extractWhitespace(const Luau::Position& newPos)
    {
        auto beginPosition = currentPosition;

        LUAU_ASSERT(currentPosition < newPos);
        LUAU_ASSERT(currentPosition.line < lineOffsets.size());
        LUAU_ASSERT(newPos.line < lineOffsets.size());
        size_t startOffset = lineOffsets[currentPosition.line] + currentPosition.column;
        size_t endOffset = lineOffsets[newPos.line] + newPos.column;

        std::string_view trivia = source.substr(startOffset, endOffset - startOffset);

        // Tokenize whitespace into smaller parts. Whitespace is separated by `\n` characters
        std::vector<Trivia> result;

        while (!trivia.empty())
        {
            auto index = trivia.find('\n');
            std::string_view part;
            if (index == std::string::npos)
                part = trivia;
            else
            {
                part = trivia.substr(0, index + 1);
                trivia.remove_prefix(index + 1);
            }

            advancePosition(part);
            result.push_back(Trivia{Trivia::Whitespace, Luau::Location{beginPosition, currentPosition}, part});
            beginPosition = currentPosition;

            if (index == std::string::npos)
                break;
        }
        LUAU_ASSERT(currentPosition == newPos);

        return result;
    }

    std::vector<Trivia> extractTrivia(const Luau::Position& newPos)
    {
        LUAU_ASSERT(currentPosition <= newPos);
        if (currentPosition == newPos)
            return {};

        std::vector<Trivia> result;

        const auto comments = commentsWithinSpan(commentLocations, Luau::Location{currentPosition, newPos});
        for (const auto& comment : comments)
        {
            if (currentPosition < comment.location.begin)
            {
                auto whitespace = extractWhitespace(comment.location.begin);
                result.insert(result.end(), whitespace.begin(), whitespace.end());
            }

            LUAU_ASSERT(comment.location.begin.line < lineOffsets.size());
            LUAU_ASSERT(comment.location.end.line < lineOffsets.size());

            size_t startOffset = lineOffsets[comment.location.begin.line] + comment.location.begin.column;
            size_t endOffset = lineOffsets[comment.location.end.line] + comment.location.end.column;

            std::string_view commentText = source.substr(startOffset, endOffset - startOffset);

            // TODO: advancePosition is more of a debug check - we can probably just set currentPosition directly here
            advancePosition(commentText);
            LUAU_ASSERT(currentPosition == comment.location.end);

            // TODO: currently the text includes the `--` / `--[[` characters, should it?
            LUAU_ASSERT(comment.type != Luau::Lexeme::BrokenComment);
            auto kind = comment.type == Luau::Lexeme::Comment ? Trivia::SingleLineComment : Trivia::MultiLineComment;
            result.emplace_back(Trivia{kind, comment.location, commentText});
        }

        if (currentPosition < newPos)
        {
            auto whitespace = extractWhitespace(newPos);
            result.insert(result.end(), whitespace.begin(), whitespace.end());
        }

        LUAU_ASSERT(currentPosition == newPos);

        return result;
    }

    // Splits a list of trivia into trailing trivia for the previos token, and leading trivia for the next token
    // The trailing trivia consists of all trivia up to and including the first '\n' character seen
    static std::pair<std::vector<Trivia>, std::vector<Trivia>> splitTrivia(std::vector<Trivia> trivia)
    {
        size_t i = 0;
        for (i = 0; i < trivia.size(); i++)
        {
            if (trivia[i].kind == Trivia::Whitespace && trivia[i].text.find('\n') != std::string::npos)
                break;
        }

        if (i == trivia.size())
            return {trivia, {}};

        auto middleIter(trivia.begin());
        std::advance(middleIter, i + 1);

        return {std::vector<Trivia>(trivia.begin(), middleIter), std::vector<Trivia>(middleIter, trivia.end())};
    }

    void serialize(Luau::Position position)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, 2);

        lua_pushnumber(L, position.line);
        lua_setfield(L, -2, "line");

        lua_pushnumber(L, position.column);
        lua_setfield(L, -2, "column");
    }

    void serialize(Luau::Location location)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, 2);

        serialize(location.begin);
        lua_setfield(L, -2, "begin");

        serialize(location.end);
        lua_setfield(L, -2, "end");
    }

    void serialize(Luau::AstName& name)
    {
        lua_rawcheckstack(L, 1);
        lua_pushstring(L, name.value);
    }

    void serialize(Luau::AstLocal* local, bool createToken = true)
    {
        lua_rawcheckstack(L, 2);

        lua_pushlightuserdata(L, local);
        lua_gettable(L, localTableIndex);

        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            lua_createtable(L, 0, 3);

            // set up reference for this local into the local table
            lua_pushlightuserdata(L, local);
            lua_pushvalue(L, -2);
            lua_settable(L, localTableIndex);

            if (createToken)
            {
                serializeToken(local->location.begin, local->name.value);
                lua_setfield(L, -2, "name");
            }

            if (local->shadow)
                serialize(local->shadow);
            else
                lua_pushnil(L);
            lua_setfield(L, -2, "shadows");

            // TODO: types
            lua_pushnil(L);
            lua_setfield(L, -2, "annotation");
        }
    }

    void serialize(Luau::AstExprTable::Item& item, Luau::CstExprTable::Item* cstNode)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, 7);

        if (item.kind == Luau::AstExprTable::Item::List)
        {
            lua_pushstring(L, "list");
            lua_setfield(L, -2, "kind");

            visit(item.value);
            lua_setfield(L, -2, "value");
        }
        else if (item.kind == Luau::AstExprTable::Item::Record)
        {
            lua_pushstring(L, "record");
            lua_setfield(L, -2, "kind");

            const auto& value = item.key->as<Luau::AstExprConstantString>()->value;
            serializeToken(item.key->location.begin, std::string(value.data, value.size).data());
            lua_setfield(L, -2, "key");

            if (cstNode && cstNode->equalsPosition)
            {
                serializeToken(cstNode->equalsPosition.value(), "=");
                lua_setfield(L, -2, "equals");
            }

            visit(item.value);
            lua_setfield(L, -2, "value");
        }
        else if (item.kind == Luau::AstExprTable::Item::General)
        {
            lua_pushstring(L, "general");
            lua_setfield(L, -2, "kind");

            if (cstNode && cstNode->indexerOpenPosition)
            {
                serializeToken(cstNode->indexerOpenPosition.value(), "[");
                lua_setfield(L, -2, "indexerOpen");
            }

            visit(item.key);
            lua_setfield(L, -2, "key");

            if (cstNode)
            {
                if (cstNode->indexerClosePosition)
                {
                    serializeToken(cstNode->indexerClosePosition.value(), "]");
                    lua_setfield(L, -2, "indexerClose");
                }

                if (cstNode->equalsPosition)
                {
                    serializeToken(cstNode->equalsPosition.value(), "=");
                    lua_setfield(L, -2, "equals");
                }
            }

            visit(item.value);
            lua_setfield(L, -2, "value");
        }

        if (cstNode && cstNode->separator)
            serializeToken(cstNode->separatorPosition.value(), cstNode->separator.value() == Luau::CstExprTable::Comma ? "," : ";");
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "separator");
    }

    void withLocation(Luau::Location location)
    {
        serialize(location);
        lua_setfield(L, -2, "location");
    }

    void serialize(Luau::AstExprBinary::Op& op)
    {
        if (op == Luau::AstExprBinary::Op::Op__Count)
            luaL_error(L, "encountered illegal operator: Op__Count");

        lua_pushstring(L, Luau::toString(op).data());
    }

    // preambleSize should encode the size of the fields we're setting up for _all_ nodes.
    static const size_t preambleSize = 2;
    void serializeNodePreamble(Luau::AstNode* node, const char* tag)
    {
        lua_rawcheckstack(L, 2);

        lua_pushstring(L, tag);
        lua_setfield(L, -2, "tag");

        withLocation(node->location);
    }

    void serializeTrivia(const std::vector<Trivia>& trivia)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, trivia.size(), 0);

        for (size_t i = 0; i < trivia.size(); i++)
        {
            lua_rawcheckstack(L, 2);
            lua_createtable(L, 0, 3);

            switch (trivia[i].kind)
            {
            case Trivia::Whitespace:
                lua_pushstring(L, "whitespace");
                break;
            case Trivia::SingleLineComment:
                lua_pushstring(L, "comment");
                break;
            case Trivia::MultiLineComment:
                lua_pushstring(L, "blockcomment");
                break;
            }
            lua_setfield(L, -2, "tag");

            serialize(trivia[i].location);
            lua_setfield(L, -2, "location");

            lua_pushlstring(L, trivia[i].text.data(), trivia[i].text.size());
            lua_setfield(L, -2, "text");

            lua_rawseti(L, -2, i + 1);
        }
    }

    // For correct trivia computation, everything must end up going through serializeToken
    void serializeToken(Luau::Position position, const char* text, int nrec = 0)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, nrec + 3);

        const auto trivia = extractTrivia(position);
        if (lastTokenRef != LUA_NOREF)
        {
            const auto [trailingTrivia, leadingTrivia] = splitTrivia(trivia);

            lua_getref(L, lastTokenRef);
            LUAU_ASSERT(lua_istable(L, -1));

            serializeTrivia(trailingTrivia);
            lua_setfield(L, -2, "trailingTrivia");
            lua_pop(L, 1);
            lua_unref(L, lastTokenRef);
            lastTokenRef = LUA_NOREF;

            serializeTrivia(leadingTrivia);
        }
        else
        {
            serializeTrivia(trivia);
        }
        LUAU_ASSERT(lua_istable(L, -2));
        lua_setfield(L, -2, "leadingTrivia");

        serialize(position);
        lua_setfield(L, -2, "position");

        lua_pushstring(L, text);
        lua_setfield(L, -2, "text");
        advancePosition(text);

        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, 0);
        lua_setfield(L, -2, "trailingTrivia");

        lastTokenRef = lua_ref(L, -1);
    }

    void serializeLocals(Luau::AstArray<Luau::AstLocal*>& locals, size_t nrec = 0)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, locals.size, nrec);

        for (size_t i = 0; i < locals.size; i++)
        {
            serialize(locals.data[i]);
            lua_rawseti(L, -2, i + 1);
        }
    }

    void serializeExprs(Luau::AstArray<Luau::AstExpr*>& exprs, size_t nrec = 0)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, exprs.size, nrec);

        for (size_t i = 0; i < exprs.size; i++)
        {
            exprs.data[i]->visit(this);
            lua_rawseti(L, -2, i + 1);
        }
    }

    void serializeStats(Luau::AstArray<Luau::AstStat*>& stats, size_t nrec = 0)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, stats.size, nrec);

        for (size_t i = 0; i < stats.size; i++)
        {
            stats.data[i]->visit(this);
            lua_rawseti(L, -2, i + 1);
        }
    }

    template<typename T>
    void serializePunctuated(Luau::AstArray<T> nodes, Luau::AstArray<Luau::Position> separators, const char* separatorText)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, nodes.size, 0);

        for (size_t i = 0; i < nodes.size; i++)
        {
            lua_rawcheckstack(L, 2);
            lua_createtable(L, 0, 2);

            nodes.data[i]->visit(this);
            lua_setfield(L, -2, "node");

            if (i < separators.size)
                serializeToken(separators.data[i], separatorText);
            else
                lua_pushnil(L);
            lua_setfield(L, -2, "separator");

            lua_rawseti(L, -2, i + 1);
        }
    }

    void serializePunctuated(Luau::AstArray<Luau::AstTypeOrPack> nodes, Luau::AstArray<Luau::Position> separators, const char* separatorText)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, nodes.size, 0);

        for (size_t i = 0; i < nodes.size; i++)
        {
            lua_rawcheckstack(L, 2);
            lua_createtable(L, 0, 2);

            if (nodes.data[i].type)
                nodes.data[i].type->visit(this);
            else
                nodes.data[i].typePack->visit(this);
            lua_setfield(L, -2, "node");

            if (i < separators.size)
                serializeToken(separators.data[i], separatorText);
            else
                lua_pushnil(L);
            lua_setfield(L, -2, "separator");

            lua_rawseti(L, -2, i + 1);
        }
    }

    void serializePunctuated(Luau::AstArray<Luau::AstLocal*> nodes, Luau::AstArray<Luau::Position> separators, const char* separatorText)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, nodes.size, 0);

        for (size_t i = 0; i < nodes.size; i++)
        {
            lua_rawcheckstack(L, 2);
            lua_createtable(L, 0, 2);

            serialize(nodes.data[i]);
            lua_setfield(L, -2, "node");

            if (i < separators.size)
                serializeToken(separators.data[i], separatorText);
            else
                lua_pushnil(L);
            lua_setfield(L, -2, "separator");

            lua_rawseti(L, -2, i + 1);
        }
    }

    void serializeEof(Luau::Position eofPosition)
    {
        serializeToken(eofPosition, "");

        lua_pushstring(L, "eof");
        lua_setfield(L, -2, "tag");
    }

    void serialize(Luau::AstExprGroup* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 3);

        serializeNodePreamble(node, "group");

        serializeToken(node->location.begin, "(");
        lua_setfield(L, -2, "openParens");

        node->expr->visit(this);
        lua_setfield(L, -2, "expression");

        serializeToken(Luau::Position{node->location.end.line, node->location.end.column - 1}, ")");
        lua_setfield(L, -2, "closeParens");
    }

    void serialize(Luau::AstExprConstantNil* node)
    {
        serializeToken(node->location.begin, "nil", preambleSize);
        serializeNodePreamble(node, "nil");
    }

    void serialize(Luau::AstExprConstantBool* node)
    {
        serializeToken(node->location.begin, node->value ? "true" : "false", preambleSize + 1);
        serializeNodePreamble(node, "boolean");

        lua_pushboolean(L, node->value);
        lua_setfield(L, -2, "value");
    }

    void serialize(Luau::AstExprConstantNumber* node)
    {
        const auto cstNode = lookupCstNode<Luau::CstExprConstantNumber>(node);

        serializeToken(node->location.begin, cstNode ? cstNode->value.data : std::to_string(node->value).data(), preambleSize + 1);
        serializeNodePreamble(node, "number");

        lua_pushnumber(L, node->value);
        lua_setfield(L, -2, "value");
    }

    void serialize(Luau::AstExprConstantString* node)
    {
        if (const auto cstNode = lookupCstNode<Luau::CstExprConstantString>(node))
        {
            serializeToken(node->location.begin, cstNode->sourceString.data, preambleSize);

            switch (cstNode->quoteStyle)
            {
            case Luau::CstExprConstantString::QuotedSingle:
                lua_pushstring(L, "single");
                break;
            case Luau::CstExprConstantString::QuotedDouble:
                lua_pushstring(L, "double");
                break;
            case Luau::CstExprConstantString::QuotedRaw:
                lua_pushstring(L, "block");
                break;
            case Luau::CstExprConstantString::QuotedInterp:
                lua_pushstring(L, "interp");
                break;
            }
            lua_setfield(L, -2, "quoteStyle");

            lua_pushnumber(L, cstNode->blockDepth);
            lua_setfield(L, -2, "blockDepth");
        }
        else
        {
            serializeToken(node->location.begin, node->value.data, preambleSize);
        }

        serializeNodePreamble(node, "string");

        // Unlike normal tokens, string content contains quotation marks that were not included during advancement
        // For simplicity, lets set the current position manually
        LUAU_ASSERT(currentPosition <= node->location.end);
        currentPosition = node->location.end;
    }

    void serialize(Luau::AstExprLocal* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 2);

        serializeNodePreamble(node, "local");

        serializeToken(node->location.begin, node->local->name.value);
        lua_setfield(L, -2, "token"),

            serialize(node->local);
        lua_setfield(L, -2, "local");

        lua_pushboolean(L, node->upvalue);
        lua_setfield(L, -2, "upvalue");
    }

    void serialize(Luau::AstExprGlobal* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 1);

        serializeNodePreamble(node, "global");

        serializeToken(node->location.begin, node->name.value);
        lua_setfield(L, -2, "name");
    }

    void serialize(Luau::AstExprVarargs* node)
    {
        serializeToken(node->location.begin, "...", preambleSize);
        serializeNodePreamble(node, "vararg");
    }

    void serialize(Luau::AstExprCall* node)
    {
        const auto cstNode = lookupCstNode<Luau::CstExprCall>(node);

        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 5);

        serializeNodePreamble(node, "call");

        node->func->visit(this);
        lua_setfield(L, -2, "func");

        if (cstNode && cstNode->openParens)
            serializeToken(*cstNode->openParens, "(");
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "openParens");

        serializePunctuated(node->args, cstNode ? cstNode->commaPositions : Luau::AstArray<Luau::Position>{}, ",");
        lua_setfield(L, -2, "arguments");

        serialize(node->argLocation);
        lua_setfield(L, -2, "argLocation");

        if (cstNode && cstNode->closeParens)
            serializeToken(*cstNode->closeParens, ")");
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "closeParens");
    }

    void serialize(Luau::AstExprIndexName* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 3);

        serializeNodePreamble(node, "indexname");

        node->expr->visit(this);
        lua_setfield(L, -2, "expr");

        serializeToken(node->opPosition, std::string(1, node->op).data());
        lua_setfield(L, -2, "accessor");

        serializeToken(node->indexLocation.begin, node->index.value);
        lua_setfield(L, -2, "index");
        serialize(node->indexLocation);
        lua_setfield(L, -2, "indexLocation");
    }

    void serialize(Luau::AstExprIndexExpr* node)
    {
        const auto* cstNode = lookupCstNode<Luau::CstExprIndexExpr>(node);

        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 2);

        serializeNodePreamble(node, "index");

        node->expr->visit(this);
        lua_setfield(L, -2, "expr");

        if (cstNode)
        {
            serializeToken(cstNode->openBracketPosition, "[");
            lua_setfield(L, -2, "openBrackets");
        }

        node->index->visit(this);
        lua_setfield(L, -2, "index");

        if (cstNode)
        {
            serializeToken(cstNode->closeBracketPosition, "]");
            lua_setfield(L, -2, "closeBrackets");
        }
    }

    void serializeFunctionBody(Luau::AstExprFunction* node)
    {
        const auto* cstNode = lookupCstNode<Luau::CstExprFunction>(node);

        lua_rawcheckstack(L, 3);
        lua_createtable(L, 0, 7);

        if (node->self)
            serialize(node->self, /* createToken= */ false);
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "self");

        if (node->argLocation)
        {
            serializeToken(node->argLocation->begin, "(");
            lua_setfield(L, -2, "openParens");
        }

        serializePunctuated(node->args, cstNode ? cstNode->argsCommaPositions : Luau::AstArray<Luau::Position>{}, ",");
        lua_setfield(L, -2, "parameters");

        // TODO: generics, return types, etc.

        if (node->vararg)
            serialize(node->varargLocation);
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "vararg");

        if (node->argLocation)
        {
            serializeToken(Luau::Position{node->argLocation->end.line, node->argLocation->end.column - 1}, ")");
            lua_setfield(L, -2, "closeParens");
        }

        node->body->visit(this);
        lua_setfield(L, -2, "body");

        serializeToken(node->body->location.end, "end");
        lua_setfield(L, -2, "end");
    }

    void serialize(Luau::AstExprFunction* node)
    {
        lua_rawcheckstack(L, 3);
        lua_createtable(L, 0, preambleSize);

        serializeNodePreamble(node, "function");

        // TODO: attributes

        serializeToken(node->location.begin, "function");
        lua_setfield(L, -2, "function");

        serializeFunctionBody(node);
        lua_setfield(L, -2, "body");
    }

    void serialize(Luau::AstExprTable* node)
    {
        const auto cstNode = lookupCstNode<Luau::CstExprTable>(node);

        lua_rawcheckstack(L, 3);
        lua_createtable(L, 0, preambleSize + 3);

        serializeNodePreamble(node, "table");

        serializeToken(node->location.begin, "{");
        lua_setfield(L, -2, "openBrace");

        lua_createtable(L, node->items.size, 0);
        for (size_t i = 0; i < node->items.size; i++)
        {
            serialize(node->items.data[i], cstNode ? &cstNode->items.data[i] : nullptr);
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "entries");

        serializeToken(Luau::Position{node->location.end.line, node->location.end.column - 1}, "}");
        lua_setfield(L, -2, "closeBrace");
    }

    void serialize(Luau::AstExprUnary* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 2);

        serializeNodePreamble(node, "unary");

        if (const auto cstNode = lookupCstNode<Luau::CstExprOp>(node))
            serializeToken(cstNode->opPosition, toString(node->op).data());
        else
            lua_pushstring(L, Luau::toString(node->op).data());
        lua_setfield(L, -2, "operator");

        node->expr->visit(this);
        lua_setfield(L, -2, "operand");
    }

    void serialize(Luau::AstExprBinary* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 3);

        serializeNodePreamble(node, "binary");

        node->left->visit(this);
        lua_setfield(L, -2, "lhsoperand");

        if (const auto cstNode = lookupCstNode<Luau::CstExprOp>(node))
            serializeToken(cstNode->opPosition, Luau::toString(node->op).data());
        else
            serialize(node->op);
        lua_setfield(L, -2, "operator");

        node->right->visit(this);
        lua_setfield(L, -2, "rhsoperand");
    }

    void serialize(Luau::AstExprTypeAssertion* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 3);

        serializeNodePreamble(node, "cast");

        node->expr->visit(this);
        lua_setfield(L, -2, "operand");

        if (const auto cstNode = lookupCstNode<Luau::CstExprTypeAssertion>(node))
        {
            serializeToken(cstNode->opPosition, "::");
            lua_setfield(L, -2, "operator");
        }

        node->annotation->visit(this);
        lua_setfield(L, -2, "annotation");
    }

    void serialize(Luau::AstExprIfElse* node)
    {
        const auto* cstNode = lookupCstNode<Luau::CstExprIfElse>(node);

        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 7);

        serializeNodePreamble(node, "conditional");

        serializeToken(node->location.begin, "if");
        lua_setfield(L, -2, "if");

        node->condition->visit(this);
        lua_setfield(L, -2, "condition");

        if (node->hasThen)
        {
            if (cstNode)
            {
                serializeToken(cstNode->thenPosition, "then");
                lua_setfield(L, -2, "then");
            }

            node->trueExpr->visit(this);
        }
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "consequent");

        lua_createtable(L, 0, preambleSize + 4);
        int i = 0;
        while (node->hasElse && node->falseExpr->is<Luau::AstExprIfElse>() && (!cstNode || cstNode->isElseIf))
        {
            lua_createtable(L, 0, 4);

            node = node->falseExpr->as<Luau::AstExprIfElse>();
            cstNode = lookupCstNode<Luau::CstExprIfElse>(node);

            serializeToken(node->location.begin, "elseif");
            lua_setfield(L, -2, "elseif");

            node->condition->visit(this);
            lua_setfield(L, -2, "condition");

            if (node->hasThen)
            {
                if (cstNode)
                {
                    serializeToken(cstNode->thenPosition, "then");
                    lua_setfield(L, -2, "then");
                }

                node->trueExpr->visit(this);
            }
            else
                lua_pushnil(L);
            lua_setfield(L, -2, "consequent");

            lua_rawseti(L, -2, i + 1);
            i++;
        }
        lua_setfield(L, -2, "elseifs");

        if (node->hasElse)
        {
            if (cstNode)
            {
                serializeToken(cstNode->elsePosition, "else");
                lua_setfield(L, -2, "else");
            }
            node->falseExpr->visit(this);
        }
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "antecedent");
    }

    void serialize(Luau::AstExprInterpString* node)
    {
        const auto* cstNode = lookupCstNode<Luau::CstExprInterpString>(node);

        lua_rawcheckstack(L, 3);
        lua_createtable(L, 0, preambleSize + 2);

        serializeNodePreamble(node, "interpolatedstring");

        lua_createtable(L, node->strings.size, 0);
        lua_createtable(L, node->expressions.size, 0);

        for (size_t i = 0; i < node->strings.size; i++)
        {
            if (cstNode)
            {
                auto position = i > 0 ? cstNode->stringPositions.data[i] : node->location.begin;
                serializeToken(position, std::string(cstNode->sourceStrings.data[i].data, cstNode->sourceStrings.data[i].size).data());

                // Unlike normal tokens, interpolated string parts contain extra characters (`, } or {) that were not included during advancement
                // For simplicity, lets set the current position manually. We don't have an end position for these parts, so we must compute
                // If string part was single line, end position = current position + 2 (start and end character)
                // If string parts was multi line, end position = current position + 1 (just end character)
                if (position.line == currentPosition.line)
                    currentPosition.column += 2;
                else
                    currentPosition.column += 1;
            }
            else
                lua_pushlstring(L, node->strings.data[i].data, node->strings.data[i].size);
            lua_rawseti(L, -3, i + 1);

            if (i < node->expressions.size)
            {
                node->expressions.data[i]->visit(this);
                lua_rawseti(L, -2, i + 1);
            }
        }
        lua_setfield(L, -3, "expressions");
        lua_setfield(L, -2, "strings");
    }

    void serialize(Luau::AstExprError* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 2);

        serializeNodePreamble(node, "error");

        serializeExprs(node->expressions);
        lua_setfield(L, -2, "expressions");

        // TODO: messageIndex reference
    }

    void serializeStat(Luau::AstStatBlock* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 1);

        serializeNodePreamble(node, "block");

        serializeStats(node->body);
        lua_setfield(L, -2, "statements");
    }

    void serializeStat(Luau::AstStatIf* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 5);

        serializeNodePreamble(node, "conditional");

        serializeToken(node->location.begin, "if");
        lua_setfield(L, -2, "if");

        node->condition->visit(this);
        lua_setfield(L, -2, "condition");

        serializeToken(node->thenLocation->begin, "then");
        lua_setfield(L, -2, "then");

        node->thenbody->visit(this);
        lua_setfield(L, -2, "consequent");

        lua_createtable(L, 0, preambleSize + 4);
        int i = 0;
        while (node->elsebody && node->elsebody->is<Luau::AstStatIf>())
        {
            lua_createtable(L, 0, 4);

            auto elseif = node->elsebody->as<Luau::AstStatIf>();
            serializeToken(elseif->location.begin, "elseif");
            lua_setfield(L, -2, "elseif");

            elseif->condition->visit(this);
            lua_setfield(L, -2, "condition");

            serializeToken(elseif->thenLocation->begin, "then");
            lua_setfield(L, -2, "then");

            elseif->thenbody->visit(this);
            lua_setfield(L, -2, "consequent");

            lua_rawseti(L, -2, i + 1);
            node = elseif;
            i++;
        }
        lua_setfield(L, -2, "elseifs");

        if (node->elsebody)
        {
            LUAU_ASSERT(node->elseLocation);
            serializeToken(node->elseLocation->begin, "else");
            lua_setfield(L, -2, "else");

            node->elsebody->visit(this);
            lua_setfield(L, -2, "antecedent");

            serializeToken(node->elsebody->location.end, "end");
            lua_setfield(L, -2, "end");
        }
        else
        {
            lua_pushnil(L);
            lua_setfield(L, -2, "else");

            lua_pushnil(L);
            lua_setfield(L, -2, "antecedent");

            serializeToken(node->thenbody->location.end, "end");
            lua_setfield(L, -2, "end");
        }
    }

    void serializeStat(Luau::AstStatWhile* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 4);

        serializeNodePreamble(node, "while");

        serializeToken(node->location.begin, "while");
        lua_setfield(L, -2, "while");

        node->condition->visit(this);
        lua_setfield(L, -2, "condition");

        if (node->hasDo)
            serializeToken(node->doLocation.begin, "do");
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "do");

        node->body->visit(this);
        lua_setfield(L, -2, "body");

        serializeToken(node->body->location.end, "end");
        lua_setfield(L, -2, "end");
    }

    void serializeStat(Luau::AstStatRepeat* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 2);

        serializeNodePreamble(node, "repeat");

        serializeToken(node->location.begin, "repeat");
        lua_setfield(L, -2, "repeat");

        node->body->visit(this);
        lua_setfield(L, -2, "body");

        if (const auto cstNode = lookupCstNode<Luau::CstStatRepeat>(node))
        {
            serializeToken(cstNode->untilPosition, "until");
            lua_setfield(L, -2, "until");
        }

        node->condition->visit(this);
        lua_setfield(L, -2, "condition");
    }

    void serializeStat(Luau::AstStatBreak* node)
    {
        lua_rawcheckstack(L, 2);
        serializeToken(node->location.begin, "break", preambleSize);
        serializeNodePreamble(node, "break");
    }

    void serializeStat(Luau::AstStatContinue* node)
    {
        lua_rawcheckstack(L, 2);
        serializeToken(node->location.begin, "continue", preambleSize);
        serializeNodePreamble(node, "continue");
    }

    void serializeStat(Luau::AstStatReturn* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 3);

        serializeNodePreamble(node, "return");

        serializeToken(node->location.begin, "return");
        lua_setfield(L, -2, "return");

        const auto cstNode = lookupCstNode<Luau::CstStatReturn>(node);
        serializePunctuated(node->list, cstNode ? cstNode->commaPositions : Luau::AstArray<Luau::Position>{}, ",");
        lua_setfield(L, -2, "expressions");
    }

    void serializeStat(Luau::AstStatExpr* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 1);

        serializeNodePreamble(node, "expression");

        node->expr->visit(this);
        lua_setfield(L, -2, "expression");
    }

    void serializeStat(Luau::AstStatLocal* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 3);

        serializeNodePreamble(node, "local");

        serializeToken(node->location.begin, "local");
        lua_setfield(L, -2, "local");

        const auto cstNode = lookupCstNode<Luau::CstStatLocal>(node);
        serializePunctuated(node->vars, cstNode ? cstNode->varsCommaPositions : Luau::AstArray<Luau::Position>{}, ",");
        lua_setfield(L, -2, "variables");

        if (node->equalsSignLocation)
            serializeToken(node->equalsSignLocation->begin, "=");
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "equals");

        serializePunctuated(node->values, cstNode ? cstNode->valuesCommaPositions : Luau::AstArray<Luau::Position>{}, ",");
        lua_setfield(L, -2, "values");
    }

    void serializeStat(Luau::AstStatFor* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 11);

        const auto cstNode = lookupCstNode<Luau::CstStatFor>(node);

        serializeNodePreamble(node, "for");

        serializeToken(node->location.begin, "for");
        lua_setfield(L, -2, "for");

        serialize(node->var);
        lua_setfield(L, -2, "variable");

        if (cstNode)
        {
            serializeToken(cstNode->equalsPosition, "=");
            lua_setfield(L, -2, "equals");
        }

        node->from->visit(this);
        lua_setfield(L, -2, "from");

        if (cstNode)
        {
            serializeToken(cstNode->endCommaPosition, ",");
            lua_setfield(L, -2, "toComma");
        }

        node->to->visit(this);
        lua_setfield(L, -2, "to");

        if (cstNode && cstNode->stepCommaPosition)
        {
            serializeToken(*cstNode->stepCommaPosition, ",");
            lua_setfield(L, -2, "stepComma");
        }

        if (node->step)
            node->step->visit(this);
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "step");

        if (node->hasDo)
            serializeToken(node->doLocation.begin, "do");
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "do");

        node->body->visit(this);
        lua_setfield(L, -2, "body");

        serializeToken(node->body->location.end, "end");
        lua_setfield(L, -2, "end");
    }

    void serializeStat(Luau::AstStatForIn* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 7);

        const auto cstNode = lookupCstNode<Luau::CstStatForIn>(node);

        serializeNodePreamble(node, "forin");

        serializeToken(node->location.begin, "for");
        lua_setfield(L, -2, "for");

        serializePunctuated(node->vars, cstNode ? cstNode->varsCommaPositions : Luau::AstArray<Luau::Position>{}, ",");
        lua_setfield(L, -2, "variables");

        if (node->hasIn)
            serializeToken(node->inLocation.begin, "in");
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "in");

        serializePunctuated(node->values, cstNode ? cstNode->valuesCommaPositions : Luau::AstArray<Luau::Position>{}, ",");
        lua_setfield(L, -2, "values");

        if (node->hasDo)
            serializeToken(node->doLocation.begin, "do");
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "do");

        node->body->visit(this);
        lua_setfield(L, -2, "body");

        serializeToken(node->body->location.end, "end");
        lua_setfield(L, -2, "end");
    }

    void serializeStat(Luau::AstStatAssign* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 3);

        const auto cstNode = lookupCstNode<Luau::CstStatAssign>(node);

        serializeNodePreamble(node, "assign");

        serializePunctuated(node->vars, cstNode ? cstNode->varsCommaPositions : Luau::AstArray<Luau::Position>{}, ",");
        lua_setfield(L, -2, "variables");

        if (cstNode)
        {
            serializeToken(cstNode->equalsPosition, "=");
            lua_setfield(L, -2, "equals");
        }

        serializePunctuated(node->values, cstNode ? cstNode->valuesCommaPositions : Luau::AstArray<Luau::Position>{}, ",");
        lua_setfield(L, -2, "values");
    }

    void serializeStat(Luau::AstStatCompoundAssign* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 3);

        serializeNodePreamble(node, "compoundassign");

        node->var->visit(this);
        lua_setfield(L, -2, "variable");

        if (const auto cstNode = lookupCstNode<Luau::CstStatCompoundAssign>(node))
            serializeToken(cstNode->opPosition, (Luau::toString(node->op) + "=").data());
        else
            serialize(node->op);
        lua_setfield(L, -2, "operand");

        node->value->visit(this);
        lua_setfield(L, -2, "value");
    }

    void serializeStat(Luau::AstStatFunction* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 3);

        serializeNodePreamble(node, "function");

        serializeToken(node->location.begin, "function");
        lua_setfield(L, -2, "function");

        node->name->visit(this);
        lua_setfield(L, -2, "name");

        serializeFunctionBody(node->func);
        lua_setfield(L, -2, "body");
    }

    void serializeStat(Luau::AstStatLocalFunction* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 4);

        serializeNodePreamble(node, "localfunction");

        serializeToken(node->location.begin, "local");
        lua_setfield(L, -2, "local");

        if (const auto cstNode = lookupCstNode<Luau::CstStatLocalFunction>(node))
        {
            serializeToken(cstNode->functionKeywordPosition, "function");
            lua_setfield(L, -2, "function");
        }

        serialize(node->name);
        lua_setfield(L, -2, "name");

        serializeFunctionBody(node->func);
        lua_setfield(L, -2, "body");
    }

    void serializeStat(Luau::AstStatTypeAlias* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 5);

        serializeNodePreamble(node, "typealias");

        const auto cstNode = lookupCstNode<Luau::CstStatTypeAlias>(node);

        if (node->exported)
            serializeToken(node->location.begin, "export");
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "export");

        serializeToken(cstNode ? cstNode->typeKeywordPosition : node->location.begin, "type");
        lua_setfield(L, -2, "typeToken");

        serializeToken(node->nameLocation.begin, node->name.value);
        lua_setfield(L, -2, "name");

        // TODO: generics

        if (cstNode)
        {
            serializeToken(cstNode->equalsPosition, "=");
            lua_setfield(L, -2, "equals");
        }

        node->type->visit(this);
        lua_setfield(L, -2, "type");
    }

    void serializeStat(Luau::AstStatTypeFunction* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 5);

        const auto cstNode = lookupCstNode<Luau::CstStatTypeFunction>(node);
        LUAU_ASSERT(cstNode); // TODO: handle non cst mode

        serializeNodePreamble(node, "typefunction");

        if (node->exported)
            serializeToken(node->location.begin, "export");
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "export");

        serializeToken(cstNode->typeKeywordPosition, "type");
        lua_setfield(L, -2, "type");

        serializeToken(cstNode->functionKeywordPosition, "function");
        lua_setfield(L, -2, "function");

        serializeToken(node->nameLocation.begin, node->name.value);
        lua_setfield(L, -2, "name");

        serializeFunctionBody(node->body);
        lua_setfield(L, -2, "body");
    }

    void serializeStat(Luau::AstStatDeclareFunction* node)
    {
        // TODO: declarations
    }

    void serializeStat(Luau::AstStatDeclareGlobal* node)
    {
        // TODO: declarations
    }

    void serializeStat(Luau::AstStatDeclareExternType* node)
    {
        // TODO: declarations
    }

    void serializeStat(Luau::AstStatError* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 3);

        serializeNodePreamble(node, "error");

        serializeExprs(node->expressions);
        lua_setfield(L, -2, "expressions");

        serializeStats(node->statements);
        lua_setfield(L, -2, "statements");

        // TODO: messageIndex reference
    }

    void serializeType(Luau::AstTypeReference* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 6);

        serializeNodePreamble(node, "reference");

        const auto cstNode = lookupCstNode<Luau::CstTypeReference>(node);

        if (node->prefix)
        {
            LUAU_ASSERT(node->prefixLocation);
            serializeToken(node->prefixLocation->begin, node->prefix->value);
            lua_setfield(L, -2, "prefix");

            if (cstNode)
            {
                LUAU_ASSERT(cstNode->prefixPointPosition);
                serializeToken(*cstNode->prefixPointPosition, ".");
                lua_setfield(L, -2, "prefixPoint");
            }
        }

        serializeToken(node->nameLocation.begin, node->name.value);
        lua_setfield(L, -2, "name");

        if (node->hasParameterList)
        {
            if (cstNode)
            {
                serializeToken(cstNode->openParametersPosition, "<");
                lua_setfield(L, -2, "openParameters");
            }

            serializePunctuated(node->parameters, cstNode ? cstNode->parametersCommaPositions : Luau::AstArray<Luau::Position>{}, ",");
            lua_setfield(L, -2, "parameters");

            if (cstNode)
            {
                serializeToken(cstNode->closeParametersPosition, ">");
                lua_setfield(L, -2, "closeParameters");
            }
        }
    }

    void serializeType(Luau::AstTypeTable* node)
    {
        const auto cstNode = lookupCstNode<Luau::CstTypeTable>(node);

        LUAU_ASSERT(cstNode); // TODO: handle non cst node

        if (cstNode->isArray)
        {
            lua_rawcheckstack(L, 2);
            lua_createtable(L, 0, preambleSize + 4);

            serializeNodePreamble(node, "array");

            serializeToken(node->location.begin, "{");
            lua_setfield(L, -2, "openBrace");

            if (node->indexer->accessLocation)
            {
                LUAU_ASSERT(node->indexer->access != Luau::AstTableAccess::ReadWrite);
                serializeToken(node->indexer->accessLocation->begin, node->indexer->access == Luau::AstTableAccess::Read ? "read" : "write");
            }
            else
                lua_pushnil(L);
            lua_setfield(L, -2, "access");

            node->indexer->resultType->visit(this);
            lua_setfield(L, -2, "type");

            serializeToken(Luau::Position{node->location.end.line, node->location.end.column - 1}, "}");
            lua_setfield(L, -2, "closeBrace");

            return;
        }

        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 4);

        serializeNodePreamble(node, "table");

        serializeToken(node->location.begin, "{");
        lua_setfield(L, -2, "openBrace");

        lua_createtable(L, cstNode->items.size, 0);
        const Luau::AstTableProp* prop = node->props.begin();
        for (size_t i = 0; i < cstNode->items.size; i++)
        {
            lua_rawcheckstack(L, 2);
            lua_createtable(L, 0, 8);

            Luau::CstTypeTable::Item item = cstNode->items.data[i];

            if (item.kind == Luau::CstTypeTable::Item::Kind::Indexer)
            {
                LUAU_ASSERT(node->indexer);

                lua_pushstring(L, "indexer");
                lua_setfield(L, -2, "kind");

                if (node->indexer->accessLocation)
                {
                    LUAU_ASSERT(node->indexer->access != Luau::AstTableAccess::ReadWrite);
                    serializeToken(node->indexer->accessLocation->begin, node->indexer->access == Luau::AstTableAccess::Read ? "read" : "write");
                }
                else
                    lua_pushnil(L);
                lua_setfield(L, -2, "access");

                serializeToken(item.indexerOpenPosition, "[");
                lua_setfield(L, -2, "indexerOpen");

                node->indexer->indexType->visit(this);
                lua_setfield(L, -2, "key");

                serializeToken(item.indexerClosePosition, "]");
                lua_setfield(L, -2, "indexerClose");

                serializeToken(item.colonPosition, ":");
                lua_setfield(L, -2, "colon");

                node->indexer->resultType->visit(this);
                lua_setfield(L, -2, "value");

                if (item.separator)
                    serializeToken(*item.separatorPosition, item.separator == Luau::CstExprTable::Comma ? "," : ";");
                else
                    lua_pushnil(L);
                lua_setfield(L, -2, "separator");
            }
            else
            {
                if (item.kind == Luau::CstTypeTable::Item::Kind::StringProperty)
                {
                    lua_pushstring(L, "stringproperty");
                    lua_setfield(L, -2, "kind");
                }
                else
                {
                    lua_pushstring(L, "property");
                    lua_setfield(L, -2, "kind");
                }

                if (prop->accessLocation)
                {
                    LUAU_ASSERT(prop->access != Luau::AstTableAccess::ReadWrite);
                    serializeToken(prop->accessLocation->begin, prop->access == Luau::AstTableAccess::Read ? "read" : "write");
                }
                else
                    lua_pushnil(L);
                lua_setfield(L, -2, "access");

                if (item.kind == Luau::CstTypeTable::Item::Kind::StringProperty)
                {
                    serializeToken(item.indexerOpenPosition, "[");
                    lua_setfield(L, -2, "indexerOpen");

                    {
                        auto initialPosition = item.stringPosition;
                        serializeToken(item.stringPosition, item.stringInfo->sourceString.data);

                        switch (item.stringInfo->quoteStyle)
                        {
                        case Luau::CstExprConstantString::QuotedSingle:
                            lua_pushstring(L, "single");
                            break;
                        case Luau::CstExprConstantString::QuotedDouble:
                            lua_pushstring(L, "double");
                            break;
                        default:
                            LUAU_ASSERT(false);
                        }
                        lua_setfield(L, -2, "quoteStyle");

                        // Unlike normal tokens, string content contains quotation marks that were not included during advancement
                        // For simplicity, lets set the current position manually
                        // If string part was single line, end position = current position + 2 (start and end character)
                        // If string parts was multi line, end position = current position + 1 (just end character)
                        if (initialPosition.line == currentPosition.line)
                            currentPosition.column += 2;
                        else
                            currentPosition.column += 1;
                    }
                    lua_setfield(L, -2, "key");

                    serializeToken(item.indexerClosePosition, "]");
                    lua_setfield(L, -2, "indexerClose");
                }
                else
                {
                    serializeToken(prop->location.begin, prop->name.value);
                    lua_setfield(L, -2, "key");
                }

                serializeToken(item.colonPosition, ":");
                lua_setfield(L, -2, "colon");

                prop->type->visit(this);
                lua_setfield(L, -2, "value");

                if (item.separator)
                    serializeToken(*item.separatorPosition, item.separator == Luau::CstExprTable::Comma ? "," : ";");
                else
                    lua_pushnil(L);
                lua_setfield(L, -2, "separator");

                ++prop;
            }
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "entries");

        serializeToken(Luau::Position{node->location.end.line, node->location.end.column - 1}, "}");
        lua_setfield(L, -2, "closeBrace");
    }

    void serializeType(Luau::AstTypeFunction* node)
    {
        // TODO: types
    }

    void serializeType(Luau::AstTypeTypeof* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 4);

        serializeNodePreamble(node, "typeof");

        serializeToken(node->location.begin, "typeof");
        lua_setfield(L, -2, "typeof");

        const auto cstNode = lookupCstNode<Luau::CstTypeTypeof>(node);
        if (cstNode)
        {
            serializeToken(cstNode->openPosition, "(");
            lua_setfield(L, -2, "openParens");
        }

        node->expr->visit(this);
        lua_setfield(L, -2, "expr");

        if (cstNode)
        {
            serializeToken(cstNode->closePosition, ")");
            lua_setfield(L, -2, "closeParens");
        }
    }

    void serializeType(Luau::AstTypeUnion* node)
    {
        const auto cstNode = lookupCstNode<Luau::CstTypeUnion>(node);

        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 4);

        serializeNodePreamble(node, "union");

        if (cstNode && cstNode->leadingPosition)
        {
            serializeToken(*cstNode->leadingPosition, "|");
            lua_setfield(L, -2, "leading");
        }

        lua_createtable(L, node->types.size, 0);
        size_t separatorPositions = 0;
        for (size_t i = 0; i < node->types.size; i++)
        {
            lua_rawcheckstack(L, 2);
            lua_createtable(L, 0, 2);

            if (node->types.data[i]->is<Luau::AstTypeOptional>())
            {
                serializeToken(node->types.data[i]->location.begin, "?", 1);
                lua_pushstring(L, "optional");
                lua_setfield(L, -2, "tag");
                lua_setfield(L, -2, "node");

                lua_pushnil(L);
                lua_setfield(L, -2, "separator");
            }
            else
            {
                node->types.data[i]->visit(this);
                lua_setfield(L, -2, "node");

                if (cstNode && i < node->types.size - 1 && !node->types.data[i+1]->is<Luau::AstTypeOptional>() && separatorPositions < cstNode->separatorPositions.size)
                    serializeToken(cstNode->separatorPositions.data[separatorPositions], "|");
                else
                    lua_pushnil(L);
                lua_setfield(L, -2, "separator");
                separatorPositions++;
            }

            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "types");
    }

    void serializeType(Luau::AstTypeIntersection* node)
    {
        const auto cstNode = lookupCstNode<Luau::CstTypeIntersection>(node);

        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 4);

        serializeNodePreamble(node, "intersection");

        if (cstNode && cstNode->leadingPosition)
        {
            serializeToken(*cstNode->leadingPosition, "&");
            lua_setfield(L, -2, "leading");
        }

        serializePunctuated(node->types, cstNode ? cstNode->separatorPositions : Luau::AstArray<Luau::Position>{}, "&");
        lua_setfield(L, -2, "types");
    }

    void serializeType(Luau::AstTypeSingletonBool* node)
    {
        serializeToken(node->location.begin, node->value ? "true" : "false", preambleSize + 1);
        serializeNodePreamble(node, "boolean");

        lua_pushboolean(L, node->value);
        lua_setfield(L, -2, "value");
    }

    void serializeType(Luau::AstTypeSingletonString* node)
    {
        if (const auto cstNode = lookupCstNode<Luau::CstTypeSingletonString>(node))
        {
            serializeToken(node->location.begin, cstNode->sourceString.data, preambleSize);

            switch (cstNode->quoteStyle)
            {
            case Luau::CstExprConstantString::QuotedSingle:
                lua_pushstring(L, "single");
                break;
            case Luau::CstExprConstantString::QuotedDouble:
                lua_pushstring(L, "double");
                break;
            default:
                LUAU_ASSERT(false);
            }
            lua_setfield(L, -2, "quoteStyle");
        }
        else
        {
            serializeToken(node->location.begin, node->value.data, preambleSize);
        }

        serializeNodePreamble(node, "string");

        // Unlike normal tokens, string content contains quotation marks that were not included during advancement
        // For simplicity, lets set the current position manually
        LUAU_ASSERT(currentPosition <= node->location.end);
        currentPosition = node->location.end;
    }

    void serializeType(Luau::AstTypeGroup* node)
    {
        lua_rawcheckstack(L, 2);
        lua_createtable(L, 0, preambleSize + 3);

        serializeNodePreamble(node, "group");

        serializeToken(node->location.begin, "(");
        lua_setfield(L, -2, "openParens");

        node->type->visit(this);
        lua_setfield(L, -2, "type");

        serializeToken(Luau::Position{node->location.end.line, node->location.end.column - 1}, ")");
        lua_setfield(L, -2, "closeParens");
    }

    void serializeType(Luau::AstTypeError* node)
    {
        // TODO: types
    }

    void serializeType(Luau::AstTypePack* node)
    {
        // TODO: types
    }

    void serializeType(Luau::AstTypePackExplicit* node)
    {
        // TODO: types
    }

    void serializeType(Luau::AstTypePackVariadic* node)
    {
        // TODO: types
    }

    void serializeType(Luau::AstTypePackGeneric* node)
    {
        // TODO: types
    }

    bool visit(Luau::AstExpr* node) override
    {
        node->visit(this);
        return false;
    }

    bool visit(Luau::AstExprGroup* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprConstantNil* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprConstantBool* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprConstantNumber* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprConstantString* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprLocal* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprGlobal* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprVarargs* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprCall* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprIndexName* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprIndexExpr* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprFunction* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprTable* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprUnary* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprBinary* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprTypeAssertion* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprIfElse* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprInterpString* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstExprError* node) override
    {
        serialize(node);
        return false;
    }

    bool visit(Luau::AstStat* node) override
    {
        node->visit(this);
        return false;
    }

    bool visit(Luau::AstStatBlock* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatIf* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatWhile* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatRepeat* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatBreak* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatContinue* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatReturn* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatExpr* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatLocal* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatFor* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatForIn* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatAssign* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatCompoundAssign* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatFunction* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatLocalFunction* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatTypeAlias* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatTypeFunction* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatDeclareFunction* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatDeclareGlobal* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatDeclareExternType* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstStatError* node) override
    {
        serializeStat(node);
        return false;
    }

    bool visit(Luau::AstType* node) override
    {
        return true;
    }

    bool visit(Luau::AstTypeReference* node) override
    {
        serializeType(node);
        return false;
    }

    bool visit(Luau::AstTypeTable* node) override
    {
        serializeType(node);
        return false;
    }

    bool visit(Luau::AstTypeFunction* node) override
    {
        return true;
    }

    bool visit(Luau::AstTypeTypeof* node) override
    {
        serializeType(node);
        return false;
    }

    bool visit(Luau::AstTypeUnion* node) override
    {
        serializeType(node);
        return false;
    }

    bool visit(Luau::AstTypeIntersection* node) override
    {
        serializeType(node);
        return false;
    }

    bool visit(Luau::AstTypeSingletonBool* node) override
    {
        serializeType(node);
        return false;
    }

    bool visit(Luau::AstTypeSingletonString* node) override
    {
        serializeType(node);
        return false;
    }

    bool visit(Luau::AstTypeError* node) override
    {
        return true;
    }

    bool visit(Luau::AstTypePack* node) override
    {
        return true;
    }

    bool visit(Luau::AstTypePackExplicit* node) override
    {
        return true;
    }

    bool visit(Luau::AstTypePackVariadic* node) override
    {
        return true;
    }

    bool visit(Luau::AstTypePackGeneric* node) override
    {
        return true;
    }

    bool visit(Luau::AstTypeGroup* node) override
    {
        serializeType(node);
        return false;
    }
};

int luau_parse(lua_State* L)
{
    std::string source = luaL_checkstring(L, 1);

    StatResult result = parse(source);

    auto& errors = result.parseResult.errors;

    if (!errors.empty())
    {
        std::vector<std::string> locationStrings{};
        locationStrings.reserve(errors.size());

        size_t size = 0;
        for (auto error : errors)
        {
            locationStrings.emplace_back(Luau::toString(error.getLocation()));
            size += locationStrings.back().size() + 2 + error.getMessage().size() + 1;
        }

        std::string fullError;
        fullError.reserve(size);

        for (size_t i = 0; i < errors.size(); i++)
        {
            fullError += locationStrings[i];
            fullError += ": ";
            fullError += errors[i].getMessage();
            fullError += "\n";
        }

        luaL_error(L, "parsing failed:\n%s", fullError.c_str());
    }

    lua_rawcheckstack(L, 3);

    lua_createtable(L, 0, 3);

    AstSerialize serializer{L, source, result.parseResult.cstNodeMap, result.parseResult.commentLocations};
    serializer.visit(result.parseResult.root);
    lua_setfield(L, -2, "root");

    serializer.serializeEof(result.parseResult.root->location.end);
    lua_setfield(L, -2, "eof");

    lua_pushnumber(L, result.parseResult.lines);
    lua_setfield(L, -2, "lines");

    return 1;
}

int luau_parseexpr(lua_State* L)
{
    std::string source = luaL_checkstring(L, 1);

    ExprResult result = parseExpr(source);

    auto& errors = result.parseResult.errors;

    if (!errors.empty())
    {
        std::vector<std::string> locationStrings{};
        locationStrings.reserve(errors.size());

        size_t size = 0;
        for (auto error : errors)
        {
            locationStrings.emplace_back(Luau::toString(error.getLocation()));
            size += locationStrings.back().size() + 2 + error.getMessage().size() + 1;
        }

        std::string fullError;
        fullError.reserve(size);

        for (size_t i = 0; i < errors.size(); i++)
        {
            fullError += locationStrings[i];
            fullError += ": ";
            fullError += errors[i].getMessage();
            fullError += "\n";
        }

        luaL_error(L, "parsing failed:\n%s", fullError.c_str());
    }

    AstSerialize serializer{L, source, result.parseResult.cstNodeMap, result.parseResult.commentLocations};
    serializer.visit(result.parseResult.expr);

    return 1;
}

inline int check_int_field(lua_State* L, int obj_idx, const char* field_name, int default_value)
{
    if (lua_getfield(L, obj_idx, field_name) == LUA_TNIL)
        return default_value;

    int is_num;
    int value = lua_tointegerx(L, -1, &is_num);

    if (!is_num)
        luaL_errorL(L, "Expected number for field \"%s\"", field_name);

    return value;
}

int compile_luau(lua_State* L)
{
    size_t source_size;
    const char* source = luaL_checklstring(L, 1, &source_size);

    Luau::CompileOptions opts{};

    if (lua_type(L, 2) == LUA_TTABLE)
    {
        opts.optimizationLevel = check_int_field(L, 2, "optimizationlevel", 1);
        opts.debugLevel = check_int_field(L, 2, "debuglevel", 1);
        opts.coverageLevel = check_int_field(L, 2, "coveragelevel", 1);
    }

    std::string bytecode = Luau::compile(std::string(source, source_size), opts);

    std::string* userdata = static_cast<std::string*>(lua_newuserdatatagged(L, sizeof(std::string), kCompilerResultTag));

    new (userdata) std::string(std::move(bytecode));

    luaL_getmetatable(L, COMPILE_RESULT_TYPE);
    lua_setmetatable(L, -2);

    return 1;
}

int load_luau(lua_State* L)
{
    const std::string* bytecode_string = static_cast<std::string*>(luaL_checkudata(L, 1, COMPILE_RESULT_TYPE));
    const char* chunk_name = luaL_optlstring(L, 2, "luau.load", nullptr);

    luau_load(L, chunk_name, bytecode_string->c_str(), bytecode_string->length(), lua_gettop(L) > 2 ? 3 : 0);

    return 1;
}

} // namespace luau

static int index_result(lua_State* L)
{
    const std::string* bytecode_string = static_cast<std::string*>(luaL_checkudata(L, 1, COMPILE_RESULT_TYPE));

    if (std::strcmp(luaL_checkstring(L, 2), "bytecode") == 0)
    {
        lua_pushlstring(L, bytecode_string->c_str(), bytecode_string->size());

        return 1;
    }

    return 0;
}

// perform type mt registration, etc
static int init_luau_lib(lua_State* L)
{
    luaL_newmetatable(L, COMPILE_RESULT_TYPE);

    lua_pushcfunction(L, index_result, "CompilerResult.__index");
    lua_setfield(L, -2, "__index");

    lua_pop(L, 1);

    return 1;
}

int luaopen_luau(lua_State* L)
{
    luaL_register(L, "luau", luau::lib);

    return init_luau_lib(L);
}

int luteopen_luau(lua_State* L)
{
    lua_createtable(L, 0, std::size(luau::lib));

    for (auto& [name, func] : luau::lib)
    {
        if (!name || !func)
            break;

        lua_pushcfunction(L, func, name);
        lua_setfield(L, -2, name);
    }

    lua_setreadonly(L, -1, 1);

    return init_luau_lib(L);
}
