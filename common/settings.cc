﻿/*  Copyright (C) 2016 Eric Wasylishen

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

 See file, 'COPYING', for details.
 */

#include "common/settings.hh"
#include "common/threads.hh"
#include "common/fs.hh"
#include <common/log.hh>

namespace settings
{
// parse_exception
parse_exception::parse_exception(std::string str) : _what(std::move(str)) { }

const char *parse_exception::what() const noexcept { return _what.c_str(); }

// nameset

nameset::nameset(const char *str) : vector<std::string>({str}) { }
nameset::nameset(const std::string &str) : vector<std::string>({str}) { }
nameset::nameset(const std::initializer_list<const char *> &strs) : vector(strs.begin(), strs.end()) { }
nameset::nameset(const std::initializer_list<std::string> &strs) : vector(strs) { }

// setting_base

setting_base::setting_base(
    setting_container *dictionary, const nameset &names, const setting_group *group, const char *description)
    : _names(names), _group(group), _description(description)
{
    Q_assert(_names.size() > 0);

    if (dictionary) {
        dictionary->registerSetting(this);
    }
}

bool setting_base::changeSource(source newSource)
{
    if (newSource >= _source) {
        _source = newSource;
        return true;
    }
    return false;
}

// setting_func

setting_func::setting_func(setting_container *dictionary, const nameset &names, std::function<void(source)> func,
    const setting_group *group, const char *description)
    : setting_base(dictionary, names, group, description), _func(func)
{
}

bool setting_func::copyFrom(const setting_base &other) { return true; }

void setting_func::reset() { }

bool setting_func::parse(const std::string &settingName, parser_base_t &parser, source source)
{
    _func(source);
    return true;
}

std::string setting_func::stringValue() const { return ""; }

std::string setting_func::format() const { return ""; }

// setting_bool

bool setting_bool::parseInternal(parser_base_t &parser, source source, bool truthValue)
{
    // boolean flags can be just flagged themselves
    if (parser.parse_token(PARSE_PEEK)) {
        // if the token that follows is 1, 0 or -1, we'll handle it
        // as a value, otherwise it's probably part of the next option.
        if (parser.token == "1" || parser.token == "0" || parser.token == "-1") {
            parser.parse_token();

            int intval = std::stoi(parser.token);

            const bool f = (intval != 0 && intval != -1) ? truthValue : !truthValue; // treat 0 and -1 as false

            setValue(f, source);

            return true;
        }
    }

    setValue(truthValue, source);

    return true;
}

setting_bool::setting_bool(setting_container *dictionary, const nameset &names, bool v,
    const setting_group *group, const char *description)
    : setting_value(dictionary, names, v, group, description)
{
}

bool setting_bool::parse(const std::string &settingName, parser_base_t &parser, source source)
{
    return parseInternal(parser, source, true);
}

std::string setting_bool::stringValue() const { return _value ? "1" : "0"; }

std::string setting_bool::format() const { return _default ? "[0]" : ""; }

// setting_invertible_bool

static nameset extendNames(const nameset &names)
{
    nameset n = names;

    for (auto &name : names) {
        n.push_back("no" + name);
    }

    return n;
}

setting_invertible_bool::setting_invertible_bool(setting_container *dictionary, const nameset &names, bool v,
    const setting_group *group, const char *description)
    : setting_bool(dictionary, extendNames(names), v, group, description)
{
}

bool setting_invertible_bool::parse(const std::string &settingName, parser_base_t &parser, source source)
{
    return parseInternal(parser, source, settingName.compare(0, 2, "no") == 0 ? false : true);
}

// setting_redirect

setting_redirect::setting_redirect(setting_container *dictionary, const nameset &names,
    const std::initializer_list<setting_base *> &settings, const setting_group *group,
    const char *description)
    : setting_base(dictionary, names, group, description), _settings(settings)
{
}

bool setting_redirect::copyFrom(const setting_base &other) { return true; }

void setting_redirect::reset() { }

bool setting_redirect::parse(const std::string &settingName, parser_base_t &parser, source source)
{
    // this is a bit ugly, but we run the parse function for
    // every setting that we redirect from. for every entry
    // except the last, we'll backup & restore the state.
    for (size_t i = 0; i < _settings.size(); i++) {
        if (i != _settings.size() - 1) {
            parser.push_state();
        }

        if (!_settings[i]->parse(settingName, parser, source)) {
            return false;
        }

        if (i != _settings.size() - 1) {
            parser.pop_state();
        }
    }

    return true;
}

std::string setting_redirect::stringValue() const { return _settings[0]->stringValue(); }

std::string setting_redirect::format() const { return _settings[0]->format(); }

// setting_group

setting_group performance_group{"Performance", 10};
setting_group logging_group{"Logging", 5};
setting_group game_group{"Game", 15};

// setting_container

setting_container::~setting_container() = default;

void setting_container::reset()
{
    for (auto setting : _settings) {
        setting->reset();
    }
}

const char *setting_base::sourceString() const
{
    switch (_source) {
        case source::DEFAULT: return "default";
        case source::GAME_TARGET: return "game target";
        case source::MAP: return "map";
        case source::COMMANDLINE: return "command line";
        default: FError("Error: unknown setting source");
    }
}

// setting_string

setting_string::setting_string(setting_container *dictionary, const nameset &names, std::string v,
    const std::string_view &format, const setting_group *group, const char *description)
    : setting_value(dictionary, names, v, group, description), _format(format)
{
}

bool setting_string::parse(const std::string &settingName, parser_base_t &parser, source source)
{
    if (parser.parse_token()) {
        setValue(parser.token, source);
        return true;
    }

    return false;
}

std::string setting_string::stringValue() const { return _value; }

std::string setting_string::format() const { return _format; }

// setting_path

setting_path::setting_path(setting_container *dictionary, const nameset &names, fs::path v,
    const setting_group *group, const char *description)
    : setting_value(dictionary, names, v, group, description)
{
}

bool setting_path::parse(const std::string &settingName, parser_base_t &parser, source source)
{
    // make sure we can parse token out
    if (!parser.parse_token()) {
        return false;
    }

    setValue(parser.token, source);
    return true;
}

std::string setting_path::stringValue() const { return _value.string(); }

std::string setting_path::format() const { return "\"relative/path\" or \"C:/absolute/path\""; }

// setting_set

setting_set::setting_set(setting_container *dictionary, const nameset &names,
    const std::string_view &format, const setting_group *group,
    const char *description)
    : setting_base(dictionary, names, group, description), _format(format)
{
}

const std::unordered_set<std::string> &setting_set::values() const { return _values; }

void setting_set::addValue(const std::string &value, source newSource)
{
    if (changeSource(newSource)) {
        _values.insert(value);
    }
}

bool setting_set::parse(const std::string &settingName, parser_base_t &parser, source source)
{
    if (!parser.parse_token(PARSE_PEEK))
        return false;

    parser.parse_token();
    addValue(parser.token, source);
    return true;
}

bool setting_set::copyFrom(const setting_base &other)
{
    if (auto *casted = dynamic_cast<const setting_set *>(&other)) {
        _values = casted->_values;
        _source = casted->_source;
        return true;
    }
    return false;
}

void setting_set::reset()
{
    _values.clear();
    _source = source::DEFAULT;
}

std::string setting_set::format() const { return _format; }

std::string setting_set::stringValue() const
{
    std::string result;

    for (auto &v : _values) {
        if (!result.empty()) {
            result += ' ';
        }

        result += '\"' + v + '\"';
    }

    return result;
}

// setting_vec3

qvec3d setting_vec3::transformVec3Value(const qvec3d &val) const
{
    return val;
}

setting_vec3::setting_vec3(setting_container *dictionary, const nameset &names, vec_t a, vec_t b, vec_t c,
    const setting_group *group, const char *description)
    : setting_value(dictionary, names, transformVec3Value({a, b, c}), group, description)
{
}

void setting_vec3::setValue(const qvec3d &f, source newsource)
{
    setting_value::setValue(transformVec3Value(f), newsource);
}

bool setting_vec3::parse(const std::string &settingName, parser_base_t &parser, source source)
{
    qvec3d vec;

    for (int i = 0; i < 3; i++) {
        if (!parser.parse_token()) {
            return false;
        }

        try {
            vec[i] = std::stod(parser.token);
        } catch (std::exception &) {
            return false;
        }
    }

    setValue(vec, source);

    return true;
}

std::string setting_vec3::stringValue() const { return qv::to_string(_value); }

std::string setting_vec3::format() const { return "x y z"; }

// setting_mangle

qvec3d setting_mangle::transformVec3Value(const qvec3d &val) const { return qv::vec_from_mangle(val); }

bool setting_mangle::parse(const std::string &settingName, parser_base_t &parser, source source)
{
    qvec3d vec {};

    for (int i = 0; i < 3; i++) {
        if (!parser.parse_token(PARSE_PEEK)) {
            break;
        }

        try {
            vec[i] = std::stod(parser.token);
        } catch (std::exception &) {
            break;
        }

        parser.parse_token();
    }

    setValue(vec, source);

    return true;
}

// setting_color

qvec3d setting_color::transformVec3Value(const qvec3d &val) const
{
    return qv::normalize_color_format(val);
}

// setting_container

void setting_container::copyFrom(const setting_container &other)
{
    for (auto setting : _settings) {
        const std::string &pri_name = setting->primaryName();
        const auto *other_setting = other.findSetting(pri_name);

        if (other_setting) {
            setting->copyFrom(*other_setting);
        }
    }
}

void setting_container::registerSetting(setting_base *setting)
{
    for (const auto &name : setting->names()) {
        Q_assert(_settingsmap.find(name) == _settingsmap.end());
        _settingsmap.emplace(name, setting);
    }

    _settings.emplace(setting);
    _groupedSettings[setting->getGroup()].insert(setting);
}

void setting_container::registerSettings(const std::initializer_list<setting_base *> &settings)
{
    registerSettings(settings.begin(), settings.end());
}

setting_base *setting_container::findSetting(const std::string &name) const
{
    // strip off leading underscores
    if (name.find("_") == 0) {
        return findSetting(name.substr(1, name.size() - 1));
    }

    if (auto it = _settingsmap.find(name); it != _settingsmap.end()) {
        return it->second;
    }

    return nullptr;
}

setting_error setting_container::setSetting(const std::string &name, const std::string &value, source source)
{
    setting_base *setting = findSetting(name);

    if (setting == nullptr) {
        if (source == source::COMMANDLINE) {
            throw parse_exception(fmt::format("Unrecognized command-line option '{}'\n", name));
        }
        return setting_error::MISSING;
    }

    parser_t p{value, { }};
    return setting->parse(name, p, source) ? setting_error::NONE : setting_error::INVALID;
}

void setting_container::setSettings(const entdict_t &epairs, source source)
{
    for (const auto &epair : epairs) {
        setSetting(epair.first, epair.second, source);
    }
}

void setting_container::printHelp()
{
    fmt::print("{}usage: {} [-help/-h/-?] [-options] {}\n\n", programDescription, programName, remainderName);

    for (auto grouped : grouped()) {
        if (grouped.first) {
            fmt::print("{}:\n", grouped.first->name);
        }

        for (auto setting : grouped.second) {
            size_t numPadding = max(static_cast<size_t>(0), 28 - (setting->primaryName().size() + 4));
            fmt::print("  -{} {:{}}    {}\n", setting->primaryName(), setting->format(), numPadding,
                setting->getDescription());

            for (int i = 1; i < setting->names().size(); i++) {
                fmt::print("   \\{}\n", setting->names()[i]);
            }
        }

        printf("\n");
    }

    throw quit_after_help_exception();
}

void setting_container::printSummary()
{
    logging::print("\n--- Options Summary ---\n");
    for (auto setting : _settings) {
        if (setting->isChanged()) {
            logging::print("    \"{}\" was set to \"{}\" (from {})\n", setting->primaryName(), setting->stringValue(),
                setting->sourceString());
        }
    }
    logging::print("\n");
}

std::vector<std::string> setting_container::parse(parser_base_t &parser)
{
    // the settings parser loop will continuously eat tokens as long as
    // it begins with a -; once we have no more settings to consume, we
    // break out of this loop and return the remainder.
    while (true) {
        // end of cmd line
        if (!parser.parse_token(PARSE_PEEK)) {
            break;
        }

        // end of options
        if (parser.token[0] != '-') {
            break;
        }

        // actually eat the token since we peeked above
        parser.parse_token();

        // remove leading hyphens. we support any number of them.
        while (parser.token.front() == '-') {
            parser.token.erase(parser.token.begin());
        }

        if (parser.token.empty()) {
            throw parse_exception("stray \"-\" in command line; please check your parameters");
        }

        if (parser.token == "help" || parser.token == "h" || parser.token == "?") {
            printHelp();
        }

        auto setting = findSetting(parser.token);

        if (!setting) {
            throw parse_exception(fmt::format("unknown option \"{}\"", parser.token));
        }

        // pass off to setting to parse; store
        // name for error message below
        std::string token = std::move(parser.token);

        if (!setting->parse(token, parser, source::COMMANDLINE)) {
            throw parse_exception(
                fmt::format("invalid value for option \"{}\"; should be in format {}", token, setting->format()));
        }
    }

    // return remainder
    std::vector<std::string> remainder;

    while (true) {
        if (parser.at_end() || !parser.parse_token()) {
            break;
        }

        remainder.emplace_back(std::move(parser.token));
    }

    return remainder;
}

// global settings
common_settings::common_settings() :
threads{
    this, "threads", 0, &performance_group, "number of threads to use, maximum; leave 0 for automatic"},
lowpriority{this, "lowpriority", true, &performance_group,
    "run in a lower priority, to free up headroom for other processes"},
log{this, "log", true, &logging_group, "whether log files are written or not"},
verbose{this, {"verbose", "v"}, false, &logging_group, "verbose output"},
nopercent{this, "nopercent", false, &logging_group, "don't output percentage messages"},
nostat{this, "nostat", false, &logging_group, "don't output statistic messages"},
noprogress{this, "noprogress", false, &logging_group, "don't output progress messages"},
nocolor{this, "nocolor", false, &logging_group, "don't output color codes (for TB, etc)"},
quiet{this, {"quiet", "noverbose"}, {&nopercent, &nostat, &noprogress}, &logging_group,
    "suppress non-important messages (equivalent to -nopercent -nostat -noprogress)"},
gamedir{this, "gamedir", "", &game_group,
    "override the default mod base directory. if this is not set, or if it is relative, it will be derived from the input file or the basedir if specified."},
basedir{this, "basedir", "", &game_group,
    "override the default game base directory. if this is not set, or if it is relative, it will be derived from the input file or the gamedir if specified."},
filepriority{this, "filepriority", search_priority_t::LOOSE,
    {{"loose", search_priority_t::LOOSE}, {"archive", search_priority_t::ARCHIVE}}, &game_group,
    "which types of archives (folders/loose files or packed archives) are higher priority and chosen first for path searching"},
paths{this, "path", "\"/path/to/folder\" <multiple allowed>", &game_group,
    "additional paths or archives to add to the search path, mostly for loose files"},
q2rtx{this, "q2rtx", false, &game_group, "adjust settings to best support Q2RTX"},
defaultpaths{this, "defaultpaths", true, &game_group,
    "whether the compiler should attempt to automatically derive game/base paths for games that support it"}
{}

void common_settings::setParameters(int argc, const char **argv)
{
    programName = fs::path(argv[0]).stem().string();
    fmt::print("---- {} / ericw-tools {} ----\n", programName, ERICWTOOLS_VERSION);
}

void common_settings::preinitialize(int argc, const char **argv) { setParameters(argc, argv); }

void common_settings::initialize(int argc, const char **argv)
{
    token_parser_t p(argc, argv, { "command line" });
    parse(p);
}

void common_settings::postinitialize(int argc, const char **argv)
{
    printSummary();

    configureTBB(threads.value(), lowpriority.value());

    if (verbose.value()) {
        logging::mask |= logging::flag::VERBOSE;
    }

    if (nopercent.value()) {
        logging::mask &= ~(bitflags<logging::flag>(logging::flag::PERCENT) | logging::flag::CLOCK_ELAPSED);
    }

    if (nostat.value()) {
        logging::mask &= ~bitflags<logging::flag>(logging::flag::STAT);
    }

    if (noprogress.value()) {
        logging::mask &= ~bitflags<logging::flag>(logging::flag::PROGRESS);
    }
    
    if (nocolor.value()) {
        logging::enable_color_codes = false;
    }
}

void common_settings::run(int argc, const char **argv)
{
    preinitialize(argc, argv);
    initialize(argc, argv);
    postinitialize(argc, argv);
}
} // namespace settings
