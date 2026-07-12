#include <catch2/catch_test_macros.hpp>

#include "video/notebooklm_gates.hpp"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

using namespace wmr;

static cv::Rect corner_mark() { return {200, 200, 60, 12}; }

TEST_CASE("NotebookLM complexity: uniform background scores ~0", "[notebooklm]") {
    cv::Mat uniform(240, 320, CV_8UC1, cv::Scalar(110));
    float s = background_complexity_score(uniform, corner_mark());
    REQUIRE(s < 1.0f);  // no gradients -> ~0
}

TEST_CASE("NotebookLM complexity: textured background scores high", "[notebooklm]") {
    cv::Mat tex(240, 320, CV_8UC1);
    cv::RNG rng(12345);
    rng.fill(tex, cv::RNG::UNIFORM, 0, 256);  // random noise -> high gradient energy
    cv::Mat uniform(240, 320, CV_8UC1, cv::Scalar(110));
    float s_textured = background_complexity_score(tex, corner_mark());
    float s_uniform = background_complexity_score(uniform, corner_mark());
    REQUIRE(s_textured > s_uniform + 20.0f);  // clearly higher
}

TEST_CASE("NotebookLM complexity: mark bbox edge excluded from the band", "[notebooklm]") {
    // Uniform background, but a high-contrast mark drawn INSIDE the bbox. The
    // band has a gap from the mark, so the mark's edge must NOT inflate the score.
    cv::Mat img(240, 320, CV_8UC1, cv::Scalar(110));
    cv::Rect mark = corner_mark();
    cv::rectangle(img, mark, cv::Scalar(255), cv::FILLED);
    float s = background_complexity_score(img, mark);
    REQUIRE(s < 1.0f);
}

TEST_CASE("NotebookLM complexity: intricate classification", "[notebooklm]") {
    cv::Rect mark = corner_mark();
    cv::Mat uniform(240, 320, CV_8UC1, cv::Scalar(110));
    REQUIRE_FALSE(background_is_intricate(uniform, mark, 10.0f));

    cv::Mat tex(240, 320, CV_8UC1);
    cv::RNG rng(777);
    rng.fill(tex, cv::RNG::UNIFORM, 0, 256);
    REQUIRE(background_is_intricate(tex, mark, 10.0f));
}

TEST_CASE("NotebookLM complexity: accepts BGR input", "[notebooklm]") {
    cv::Mat uniform_bgr(240, 320, CV_8UC3, cv::Scalar(110, 120, 130));
    float s = background_complexity_score(uniform_bgr, corner_mark());
    REQUIRE(s < 1.0f);
}

TEST_CASE("NotebookLM method routing: resolve_inpaint_method", "[notebooklm]") {
    const double thr = 15.0;       // fsr/complexity threshold (--complexity-threshold)
    const double lama_thr = 60.0;  // LaMa threshold (--lama-threshold)
    const float uniform = 5.0f;    // below fsr threshold (cinematic-like)
    const float intricate = 41.0f; // >= fsr, < lama (Neon-like explainer)
    const float hard = 125.0f;     // >= lama (Arcade scene-18 cartoon)

    SECTION("auto + uniform -> ns") {
        REQUIRE(resolve_inpaint_method(uniform, thr, lama_thr, "auto", true, true) == "ns");
        REQUIRE(resolve_inpaint_method(uniform, thr, lama_thr, "auto", false, false) == "ns");
    }
    SECTION("auto + intricate + xphoto -> fsr") {
        REQUIRE(resolve_inpaint_method(intricate, thr, lama_thr, "auto", true, true) == "fsr");
    }
    SECTION("auto + intricate + NO xphoto -> ns (no regression vs v1.6.0 NS default)") {
        REQUIRE(resolve_inpaint_method(intricate, thr, lama_thr, "auto", false, true) == "ns");
    }
    SECTION("auto NEVER selects LaMa, even on the hardest scene with LaMa built") {
        // LaMa is ~2.4 s/frame — opt-in only; auto stays FSR/NS.
        REQUIRE(resolve_inpaint_method(hard, thr, lama_thr, "auto", true, true) == "fsr");
        REQUIRE(resolve_inpaint_method(hard, thr, lama_thr, "auto", false, true) == "ns");
    }
    SECTION("explicit ns -> ns regardless of complexity/xphoto/lama") {
        REQUIRE(resolve_inpaint_method(hard, thr, lama_thr, "ns", true, true) == "ns");
        REQUIRE(resolve_inpaint_method(uniform, thr, lama_thr, "ns", false, false) == "ns");
    }
    SECTION("explicit fsr honoured only when xphoto is compiled in") {
        REQUIRE(resolve_inpaint_method(intricate, thr, lama_thr, "fsr", true, true) == "fsr");
        REQUIRE(resolve_inpaint_method(uniform, thr, lama_thr, "fsr", true, true) == "fsr");
        REQUIRE(resolve_inpaint_method(intricate, thr, lama_thr, "fsr", false, true) == "ns");
    }
    SECTION("complexity == fsr threshold counts as intricate (>=)") {
        REQUIRE(resolve_inpaint_method(static_cast<float>(thr), thr, lama_thr, "auto", true, true) == "fsr");
    }
    SECTION("unrecognized requested value behaves like auto (safe)") {
        REQUIRE(resolve_inpaint_method(intricate, thr, lama_thr, "bogus", true, true) == "fsr");
        REQUIRE(resolve_inpaint_method(uniform, thr, lama_thr, "bogus", true, true) == "ns");
    }

    SECTION("lama + hard scene + LaMa built -> lama") {
        REQUIRE(resolve_inpaint_method(hard, thr, lama_thr, "lama", true, true) == "lama");
    }
    SECTION("lama + hard scene + NO LaMa -> falls back to fsr (intricate + xphoto)") {
        REQUIRE(resolve_inpaint_method(hard, thr, lama_thr, "lama", true, false) == "fsr");
    }
    SECTION("lama + hard scene + NO LaMa + NO xphoto -> ns") {
        REQUIRE(resolve_inpaint_method(hard, thr, lama_thr, "lama", false, false) == "ns");
    }
    SECTION("lama + intricate (below lama threshold) -> fsr, not lama") {
        REQUIRE(resolve_inpaint_method(intricate, thr, lama_thr, "lama", true, true) == "fsr");
    }
    SECTION("lama + uniform (below fsr threshold) -> ns") {
        REQUIRE(resolve_inpaint_method(uniform, thr, lama_thr, "lama", true, true) == "ns");
    }
    SECTION("complexity == lama threshold counts as hard (>=) -> lama") {
        REQUIRE(resolve_inpaint_method(static_cast<float>(lama_thr), thr, lama_thr, "lama", true, true) == "lama");
    }
}
