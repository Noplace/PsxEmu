// filter_chain_test - the custom filter chain's arithmetic: PSXEmu.Win32/graphics/filter_chain.h,
// which turns the stages chosen in Settings > Video into the passes an engine runs, and
// shader_pass.h's rule for what an engine will run. Header-only, like dlss_choice_test; nothing
// of the front end is linked. Whether the picture each makes looks right is a person's to judge.

#include "graphics/filter_chain.h"
#include "psx/settings.h"

#include <cstdio>
#include <string>
#include <vector>

using psxemu::FilterChainPlan;
using psxemu::PlanFilterChain;
using psxemu::ShaderPass;

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char* what) {
  ++checks;
  printf("  %-72s %s\n", what, ok ? "ok" : "FAILED");
  if (!ok)
    ++failures;
}

bool Is(const ShaderPass& pass, const char* shader, int scale, int original) {
  return pass.shader == shader && pass.scale == scale && pass.original == original;
}

void TestPlans() {
  printf("plans\n");
  Check(PlanFilterChain({}).passes.empty(), "no stages, no passes: the picture plain");

  {
    const FilterChainPlan plan = PlanFilterChain({ "scanline" });
    Check(plan.passes.size() == 1 && Is(plan.passes[0], "scanline", 0, -1),
          "one filter alone draws into the window, as it does without a chain");
  }
  {
    // Super-xBR alone is the chain the Filter list has always had for it.
    const FilterChainPlan plan = PlanFilterChain({ "superxbr" });
    Check(plan.passes.size() == 3 && Is(plan.passes[0], "superxbr_pass0", 2, -1) &&
              Is(plan.passes[1], "superxbr_pass1", 2, -1) &&
              Is(plan.passes[2], "superxbr_pass2", 2, -1),
          "Super-xBR alone: three passes at 2x, the frame their original");
    Check(psxemu::IsRunnableChain(plan.passes), "and an engine will run it");
  }
  {
    const FilterChainPlan plan = PlanFilterChain({ "superxbr", "scanline" });
    Check(plan.passes.size() == 4 && Is(plan.passes[3], "scanline", 0, -1),
          "Super-xBR then Scanline: Scanline last, into the window");
    Check(psxemu::IsRunnableChain(plan.passes), "runnable");
  }
  {
    // xBRZ doubles; Super-xBR after it doubles again and looks back at xBRZ's picture.
    const FilterChainPlan plan = PlanFilterChain({ "xbrz", "superxbr" });
    Check(plan.passes.size() == 4 && Is(plan.passes[0], "xbrz", 2, -1) &&
              Is(plan.passes[1], "superxbr_pass0", 4, 0) &&
              Is(plan.passes[2], "superxbr_pass1", 4, 0) &&
              Is(plan.passes[3], "superxbr_pass2", 4, 0),
          "xBRZ then Super-xBR: 2x, then 4x with xBRZ's picture as the original");
    Check(psxemu::IsRunnableChain(plan.passes), "runnable");
  }
  {
    const FilterChainPlan plan = PlanFilterChain({ "crt", "eagle" });
    Check(plan.passes.size() == 2 && Is(plan.passes[0], "crt", 1, -1) &&
              Is(plan.passes[1], "eagle", 0, -1),
          "a filter that keeps the size, first: at 1x, then the upscaler into the window");
  }
  {
    const FilterChainPlan plan = PlanFilterChain({ "xbrz", "superxbr", "eagle", "scanline" });
    Check(plan.dropped.size() == 1 && plan.dropped[0] == "eagle",
          "a third upscaler would pass 4x: left out, and said to be");
    Check(plan.passes.size() == 5 && Is(plan.passes[4], "scanline", 0, -1) &&
              psxemu::IsRunnableChain(plan.passes),
          "and the rest still runs, Scanline last");
  }
  {
    const FilterChainPlan plan = PlanFilterChain({ "", "chain", "bilinear" });
    Check(plan.passes.size() == 1 && Is(plan.passes[0], "bilinear", 0, -1),
          "None and the chain itself are no stage");
  }
}

void TestRunnable() {
  printf("what an engine runs\n");
  Check(!psxemu::IsRunnableChain({}), "an empty chain is refused");
  Check(!psxemu::IsRunnableChain({ { "a", 0, -1 }, { "b", 0, -1 } }),
        "only the last pass may draw into the window");
  Check(!psxemu::IsRunnableChain({ { "a", 2, -1 }, { "b", 2, 1 } }),
        "an original must be an earlier pass");
  Check(!psxemu::IsRunnableChain({ { "a", -1, -1 } }), "a negative scale is refused");
  Check(!psxemu::IsRunnableChain({ { "a", 2, -2 } }), "an original below -1 is refused");
  Check(psxemu::IsRunnableChain({ { "a", 2, -1 }, { "b", 4, 0 }, { "c", 0, 1 } }),
        "earlier passes as originals are fine");
}

void TestSettings() {
  printf("settings\n");
  using emulation::psx::EmuConfig;
  using emulation::psx::IsValidFilterChain;
  using emulation::psx::SettingsFile;
  using emulation::psx::SplitFilterChain;

  Check(SplitFilterChain("").empty(), "an empty chain has no stages");
  Check(SplitFilterChain("xbrz,scanline") == std::vector<std::string>({ "xbrz", "scanline" }),
        "stages split at the commas");
  Check(IsValidFilterChain("") && IsValidFilterChain("superxbr,crt") &&
            IsValidFilterChain("xbrz,xbrz,eagle,scanline"),
        "up to four filters is a chain");
  Check(!IsValidFilterChain("xbrz,xbrz,eagle,scanline,crt"), "five is not");
  Check(!IsValidFilterChain("xbrz,,crt") && !IsValidFilterChain(",crt") &&
            !IsValidFilterChain("crt,"),
        "an empty stage is not");
  Check(!IsValidFilterChain("chain") && !IsValidFilterChain("xbrz,sepia"),
        "the chain itself, or a filter there is not, is not");

  EmuConfig config;
  Check(config.filter_chain.empty() && config.frame_pacing == "console",
        "no chain and the console's own rate by default");
  config.video_filter = "chain";
  config.filter_chain = "superxbr,scanline";
  config.frame_pacing = "display";
  SettingsFile out;
  emulation::psx::StoreConfig(out, config);
  EmuConfig loaded;
  emulation::psx::LoadConfig(out, loaded);
  Check(loaded.video_filter == "chain" && loaded.filter_chain == "superxbr,scanline" &&
            loaded.frame_pacing == "display",
        "the chain as the filter, its stages and Frame Pacing survive the round trip");
  out.SetString("filter_chain", "superxbr,sepia");
  out.SetString("frame_pacing", "gsync");
  EmuConfig rejected;
  emulation::psx::LoadConfig(out, rejected);
  Check(rejected.filter_chain.empty() && rejected.frame_pacing == "console",
        "a chain or a pacing the window does not offer is ignored");
}

}  // namespace

int main() {
  printf("filter chain\n");
  TestPlans();
  TestRunnable();
  TestSettings();
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
