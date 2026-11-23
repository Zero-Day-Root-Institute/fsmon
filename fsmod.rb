class Fsmod < Formula
  # Compatibility recipe for the upstream release, not this checkout's daemon.
  desc "File-system monitor supporting inotify, fanotify, kqueue, fsevents"
  homepage "https://github.com/nowsecure/fsmon"
  head "https://github.com/nowsecure/fsmon.git", branch: "master"
  url "https://github.com/nowsecure/fsmon/archive/refs/tags/1.8.8.tar.gz"
  sha256 "b7f9a7310d7721c58e6e5bc432c5b2099be55b374ab9bf5f639420d2b9e0ab0a"
  license "MIT"

  def install
    system "make"
    bin.install "fsmon-macos" => "fsmod"
    man1.install "fsmon.1" => "fsmod.1"
  end

  test do
    assert_match "Usage:", shell_output("#{bin}/fsmod -h")
  end
end
