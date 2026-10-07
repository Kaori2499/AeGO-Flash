#include "MotionJson.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int checks = 0;
void expect(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
std::string fixture(const std::string& segments, int segmentCount, int pointCount) {
    return "{\"Version\":3,\"Meta\":{\"Duration\":3,\"Fps\":30,\"Loop\":false,"
        "\"AreBeziersRestricted\":true,\"CurveCount\":1,\"TotalSegmentCount\":" + std::to_string(segmentCount) +
        ",\"TotalPointCount\":" + std::to_string(pointCount) + ",\"UserDataCount\":0,\"TotalUserDataSize\":0},"
        "\"Curves\":[{\"Target\":\"Parameter\",\"Id\":\"ParamPedalOn\",\"Segments\":[" + segments + "]}]}";
}
l2dae::MotionJsonResult normalize(const std::string& text) {
    return l2dae::normalizeMotionJson({text.begin(), text.end()});
}
std::string compact(const l2dae::MotionJsonResult& result) {
    std::string output; bool quoted = false, escaped = false;
    for (const auto byte : result.bytes) {
        const char c = static_cast<char>(byte);
        if (quoted) {
            output += c;
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') { quoted = true; output += c; }
        else if (c != '\n' && c != '\r' && c != '\t' && c != ' ') output += c;
    }
    return output;
}
std::string replace(std::string text, const std::string& before, const std::string& after) {
    const auto at = text.find(before);
    if (at == std::string::npos) throw std::runtime_error("bad test replacement");
    text.replace(at, before.size(), after); return text;
}
void rejects(const std::string& input, const std::string& diagnostic) {
    try { normalize(input); }
    catch (const std::runtime_error& error) {
        expect(std::string(error.what()).find(diagnostic) != std::string::npos,
            "wrong diagnostic: " + std::string(error.what()) + "; expected " + diagnostic);
        return;
    }
    expect(false, "invalid input accepted; expected " + diagnostic);
}
std::vector<double> outputSegments(const std::string& json) {
    const std::string prefix = "\"Segments\":[";
    const auto at = json.find(prefix);
    if (at == std::string::npos) throw std::runtime_error("missing output segments");
    const char* current = json.c_str() + at + prefix.size();
    std::vector<double> result;
    for (;;) {
        char* end = nullptr;
        result.push_back(std::strtod(current, &end));
        if (end == current) throw std::runtime_error("bad output number");
        if (*end == ']') return result;
        if (*end != ',') throw std::runtime_error("bad output delimiter");
        current = end + 1;
    }
}
// Independent evaluation of the resulting standard linear/stepped segments.
double sample(const std::vector<double>& curve, double time) {
    double startTime = curve.at(0), startValue = curve.at(1);
    for (std::size_t i = 2; i < curve.size(); i += 3) {
        const int type = static_cast<int>(curve.at(i));
        if (type != 0 && type != 2) throw std::runtime_error("unexpected output segment type");
        const double endTime = curve.at(i + 1), endValue = curve.at(i + 2);
        if (time < endTime) return type == 2 ? startValue :
            startValue + (endValue - startValue) * (time - startTime) / (endTime - startTime);
        startTime = endTime; startValue = endValue;
    }
    return startValue;
}
}

int main() {
    try {
        const auto standard = fixture("0,1,1,1,2,2,3,3,4", 1, 4);
        const auto normal = normalize(standard);
        expect(normal.convertedStepSegments == 0, "ordinary Bezier counted as repaired");
        expect(compact(normal) == standard, "ordinary Bezier or metadata changed");
        const auto crossed = replace(fixture("0,0,1,2.7,1,0.3,2,3,3", 1, 4),
            "\"AreBeziersRestricted\":true", "\"AreBeziersRestricted\":false");
        expect(compact(normalize(crossed)) == crossed, "finite unrestricted crossed control times were rejected or changed");
        expect(compact(normalize(std::string("\xef\xbb\xbf") + standard)) == standard, "UTF-8 BOM rejected");
        const auto exponent = normalize(fixture("0e0,1E+0,0,3e0,2E-1", 1, 2));
        expect(compact(exponent).find("\"Segments\":[0,1,0,3,0.2]") != std::string::npos, "finite exponents not normalized");
        expect(exponent.convertedStepSegments == 0, "finite exponent counted as repair");
        expect(compact(normalize(fixture("0,1,2,1,2,3,2,4,0,3,5", 3, 4))).find(
            "\"Segments\":[0,1,2,1,2,3,2,4,0,3,5]") != std::string::npos, "standard stepped types changed");

        for (const char* infinity : {"-Infinity", "Infinity", "+Infinity"}) {
            const auto original = fixture(std::string("0,2,0,1,4,1,1.333,4,1.666,") + infinity + ",2,8,0,3,10", 3, 6);
            const auto converted = normalize(original);
            const auto json = compact(converted);
            expect(converted.convertedStepSegments == 1, "recognized tangent was not converted");
            expect(json.find("\"TotalSegmentCount\":3,\"TotalPointCount\":4") != std::string::npos, "wrong reduced point count");
            expect(json.find("\"Segments\":[0,2,0,1,4,2,2,8,0,3,10]") != std::string::npos, "keyframe endpoints or neighboring segments changed");
            const auto curve = outputSegments(json);
            for (const auto& test : std::vector<std::pair<double, double>>{{0,2},{0.5,3},{1,4},{1.5,4},{1.999999,4},{2,8},{2.5,9},{3,10}})
                expect(std::abs(sample(curve, test.first) - test.second) < 1e-9, "step boundary or neighboring segment evaluation changed");
            const auto again = normalize(std::string(converted.bytes.begin(), converted.bytes.end()));
            expect(again.convertedStepSegments == 0 && compact(again) == json, "normalization is not idempotent");
        }
        const auto repeated = normalize(fixture("0,1,1,0.333,1,0.666,-Infinity,1,0,1,1.333,0,1.666,Infinity,2,1,0,3,1", 3, 8));
        expect(repeated.convertedStepSegments == 2, "multiple repairs not counted");
        expect(compact(repeated).find("\"TotalPointCount\":4") != std::string::npos, "multiple repairs have incorrect point count");
        expect(compact(repeated).find("\"Segments\":[0,1,2,1,0,2,2,1,0,3,1]") != std::string::npos, "consecutive steps not preserved");
        const auto flat = normalize(fixture("0,1,1,1,1,2,-Infinity,3,1", 1, 4));
        expect(compact(flat).find("\"Segments\":[0,1,2,3,1]") != std::string::npos, "constant-keyframe pattern not repaired");

        rejects(fixture("0,1,1,1,2,2,-Infinity,3,4", 1, 4), "first control value");
        rejects(fixture("0,1,1,1,1,2,NaN,3,4", 1, 4), "malformed JSON");
        rejects(fixture("0,1,1,Infinity,1,2,1,3,4", 1, 4), "must be finite");
        rejects(fixture("0,1,1,1,Infinity,2,1,3,4", 1, 4), "must be finite");
        rejects(fixture("0,1,1,1,1,2,-Infinity,3,Infinity", 1, 4), "must be finite");
        rejects(fixture("0,Infinity,0,3,4", 1, 2), "initial time and value");
        rejects(fixture("0,1,0,3,-Infinity", 1, 2), "must be finite");
        rejects(fixture("0,1,4,3,4", 1, 2), "unknown segment type");
        rejects(fixture("0,1,1.5,3,4", 1, 2), "unknown segment type");
        rejects(fixture("0,1,1,1,1,2,-Infinity,3", 1, 4), "truncated segment");
        rejects(fixture("0,1", 0, 1), "contains no segment");
        rejects(fixture("0,1,0,0,1", 1, 2), "times must increase");
        rejects(fixture("0,1,1,2,1,1,-Infinity,3,4", 1, 4), "control times");
        rejects(fixture("0,1,1,1,1,2,-Infinity,3,4", 2, 4), "TotalSegmentCount");
        rejects(fixture("0,1,1,1,1,2,-Infinity,3,4", 1, 2), "original points");
        rejects(replace(standard, "\"CurveCount\":1", "\"CurveCount\":2"), "CurveCount");
        rejects(replace(standard, "\"TotalPointCount\":4", "\"TotalPointCount\":-1"), "TotalPointCount");
        rejects(replace(standard, "\"TotalPointCount\":4", "\"TotalPointCount\":4.5"), "TotalPointCount");
        rejects(replace(standard, "\"Version\":3", "\"Version\":4"), "Version");
        rejects(replace(standard, "\"Duration\":3", "\"Duration\":Infinity"), "Duration");
        rejects(replace(standard, "\"Duration\":3", "\"Duration\":0"), "Duration");
        rejects(replace(standard, "\"Fps\":30", "\"Fps\":0"), "Fps");
        rejects(replace(standard, "\"Version\":3", "\"Version\":3,\"Version\":3"), "duplicate object key");
        rejects(replace(standard, "\"Version\":3", "\"Version\":3,\"\\u0056ersion\":3"), "duplicate object key");
        rejects(replace(standard, "\"Version\":3", "\"Version\":3,\"Extra\":Infinity"), "invalid value elsewhere");
        rejects(replace(standard, "\"UserDataCount\":0", "\"UserDataCount\":1"), "UserDataCount");
        rejects(fixture("0,1,0,3,1e99", 1, 2), "32-bit values");
        rejects(fixture("0,1,0,3,1e-300", 1, 2), "32-bit values");
        rejects(standard + " garbage", "malformed JSON");
        rejects(replace(standard, "\"Fps\":30", "\"Fps\":03"), "malformed JSON");
        rejects(replace(standard, "\"Fps\":30", "\"Fps\":3e"), "malformed JSON");
        rejects(replace(standard, "\"Fps\":30", "\"Fps\":3."), "malformed JSON");
        const auto literal = replace(standard, "ParamPedalOn", "literal Infinity and \\\"quote\\\"");
        expect(compact(normalize(literal)) == literal, "Infinity inside strings was modified");
        const auto unicode = normalize(replace(standard, "ParamPedalOn", "\\u773c\\u775b"));
        expect(compact(unicode).find("眼睛") != std::string::npos, "Unicode ID was not decoded safely");
        const auto objectEnd = l2dae::terminateCubismNumbers(R"({"Y":-1.0})");
        expect(objectEnd.find("-1.0\n}") != std::string::npos, "compact number before object end");
        const auto arrayEnd = l2dae::terminateCubismNumbers("[1,2]");
        expect(arrayEnd.find("2\n]") != std::string::npos, "compact number before array end");
        const auto spaced = l2dae::terminateCubismNumbers(R"({"Y":-1.0 })");
        expect(spaced.find("-1.0\n }") != std::string::npos, "newline precedes whitespace before a closer");
        const std::string quoted = l2dae::terminateCubismNumbers(R"({"Name":"1}"})");
        expect(quoted == "{\"Name\":\"1}\"}", "digits inside strings stay intact");
        const std::string pretty = l2dae::terminateCubismNumbers("{\"Y\": -1.0,\n}");
        expect(pretty == "{\"Y\": -1.0,\n}", "a comma already ends the number");
        std::cout << "Motion JSON compatibility: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED after " << checks << " checks: " << error.what() << '\n'; return 1;
    }
}
