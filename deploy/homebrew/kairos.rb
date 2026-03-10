# File: deploy/homebrew/kairos.rb
#
# Homebrew formula for Kairos.
#
# Install:  brew install kairos  (from tap)
#           brew install --build-from-source kairos
#
# Spec reference: §28.7

class Kairos < Formula
  desc "Unified orchestration daemon — scheduling, workflows, and FS monitoring"
  homepage "https://github.com/araray/kairos"
  url "https://github.com/araray/kairos/archive/refs/tags/v2.0.0.tar.gz"
  sha256 "PLACEHOLDER_SHA256"  # Computed at release time.
  license "MIT"
  head "https://github.com/araray/kairos.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "sqlite"

  def install
    system "cmake", "-S", ".", "-B", "build",
           "-DCMAKE_BUILD_TYPE=Release",
           "-DKAIROS_HTTP=ON",
           "-DKAIROS_BUILD_TESTS=OFF",
           *std_cmake_args
    system "cmake", "--build", "build", "--parallel"
    system "cmake", "--install", "build"

    # Install launchd plist.
    prefix.install "deploy/launchd/com.kairos.daemon.plist"

    # Install example config.
    (etc/"kairos").install "deploy/kairos.toml.example" => "kairos.toml"

    # Generate and install shell completions.
    bash_completion.install Utils.safe_popen_read(
      bin/"kairos", "completions", "bash"
    ).to_s => "kairos"
    zsh_completion.install Utils.safe_popen_read(
      bin/"kairos", "completions", "zsh"
    ).to_s => "_kairos"
    fish_completion.install Utils.safe_popen_read(
      bin/"kairos", "completions", "fish"
    ).to_s => "kairos.fish"
  end

  service do
    run [opt_bin/"kairos", "start", "--config", etc/"kairos/kairos.toml"]
    keep_alive true
    log_path var/"log/kairos/kairos.log"
    error_log_path var/"log/kairos/kairos.error.log"
    working_dir var/"lib/kairos"
  end

  def post_install
    (var/"lib/kairos").mkpath
    (var/"log/kairos").mkpath
  end

  test do
    assert_match "Kairos v", shell_output("#{bin}/kairos version")

    # Verify init-db creates a database.
    system bin/"kairos", "init-db", "--db-path", testpath/"test.db"
    assert_predicate testpath/"test.db", :exist?
  end
end
