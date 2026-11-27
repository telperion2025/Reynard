# Homebrew formula for Reynard (spec 13). Install with:
#   brew install ./packaging/homebrew/reynard.rb   (from the repo root)
class Reynard < Formula
  desc "Cross-platform terminal reimplementation of FoxPro 2.x/xBase"
  homepage "https://github.com/telperion2025/Reynard"
  url "https://github.com/telperion2025/Reynard/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "REPLACE_WITH_SHA256_OF_RELEASE_TARBALL"
  license "BSD-2-Clause"
  head "https://github.com/telperion2025/Reynard.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "ncurses"

  def install
    system "cmake", "-B", "build", "-DCMAKE_BUILD_TYPE=Release",
                    "-DRX_WERROR=on", "-DUSE_SYSTEM_NCURSES=on", *std_cmake_args
    system "cmake", "--build", "build", "--parallel"
    system "cmake", "--install", "build"
    # also install the plain-make build path used by BSD ports
    # man page and docs are installed by the cmake install rules above
  end

  test do
    assert_match "reynard 1.0.0", shell_output("#{bin}/reynard --version")
    system bin/"reynard", "-t", "-c", "? 1+1"
  end
end
