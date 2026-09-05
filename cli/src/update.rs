//! Updating Astral.
//!
//! Most people who run this installed a release and have no source tree, so the
//! default is to fetch one. A release carries a build for each platform it was
//! made for, so the normal path downloads the one that matches this machine and
//! puts it in place; building from source is what happens when there is no such
//! build, or when it is asked for. A local tree is used only when asked for,
//! and is found by looking around rather than by a path written in at build
//! time.

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use crate::out::{print, Sink, Stream};
use crate::paths::{default_prefix, install_tree_writable, SYSTEM_PREFIX};

const PROJECT: &str = "Hexadecimall/Astral";
const GHIDRA_REPO: &str = "https://github.com/NationalSecurityAgency/ghidra";

const HELP: &str = concat!(
    "usage: astral update [install] [options]\n",
    "\n",
    "Installs the newest release of Astral. A release carries a build for\n",
    "each platform, so this normally downloads the one made for this machine\n",
    "and puts it in place; with --source, or where no such build exists, it\n",
    "builds from source instead.\n",
    "\n",
    "  install              install into /usr/local rather than over the\n",
    "                       copy that is running\n",
    "      --check          compare what is installed against the newest\n",
    "      --release <tag>  a particular release rather than the newest\n",
    "      --source         build from source rather than take the build\n",
    "                       that was made for this machine\n",
    "      --local          build a source tree here, rather than download\n",
    "      --ghidra <ver>   vendor that Ghidra release first\n",
    "      --languages <l>  processors to compile specs for, or ALL\n",
    "      --prefix <dir>   install somewhere else\n",
    "      --jobs <n>       parallel build jobs\n",
    "      --keep           leave the downloaded tree in place\n",
);

pub fn usage(stream: Stream) -> i32 {
    Sink::new(stream).write(HELP);
    stream.code()
}

// ---- running other programs ---------------------------------------------

/// Echoes what is about to happen, then does it. Arguments are passed as
/// themselves rather than through a shell, so nothing has to be quoted.
fn run(program: &str, arguments: &[&str]) -> i32 {
    announce(program, arguments);
    match Command::new(program).args(arguments).status() {
        Ok(status) if status.success() => 0,
        _ => 1,
    }
}

fn announce(program: &str, arguments: &[&str]) {
    let line = std::iter::once(program.to_string())
        .chain(arguments.iter().map(|a| a.to_string()))
        .collect::<Vec<_>>()
        .join(" ");
    Sink::new(Stream::Err).write(&format!("==> {line}\n"));
}

/// Standard output of a command, or nothing when it could not be run.
fn capture(program: &str, arguments: &[&str]) -> String {
    match Command::new(program)
        .args(arguments)
        .stderr(Stdio::null())
        .output()
    {
        Ok(output) => String::from_utf8_lossy(&output.stdout).into_owned(),
        Err(_) => String::new(),
    }
}

fn have(program: &str) -> bool {
    Command::new(program)
        .arg("--version")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|status| status.success())
        .unwrap_or(false)
}

// ---- reading what GitHub says -------------------------------------------

/// The string value of a JSON field, without pulling in a parser for the two
/// fields this needs.
fn json_field(body: &str, name: &str) -> String {
    let key = format!("\"{name}\"");
    let Some(at) = body.find(&key) else {
        return String::new();
    };
    let rest = &body[at + key.len()..];
    let Some(colon) = rest.find(':') else {
        return String::new();
    };
    let rest = &rest[colon..];
    let Some(quote) = rest.find('"') else {
        return String::new();
    };

    let mut value = String::new();
    let mut characters = rest[quote + 1..].chars();
    while let Some(character) = characters.next() {
        match character {
            '\\' => {
                if let Some(escaped) = characters.next() {
                    value.push(escaped);
                }
            }
            '"' => break,
            _ => value.push(character),
        }
    }
    value
}

/// The newest published version and where to get it. A project with no releases
/// still has tags, and failing that a default branch.
struct Release {
    tag: String,
    tarball: String,
}

fn newest_release(wanted: &str) -> Release {
    let archive = format!("https://github.com/{PROJECT}/archive/refs/");
    if !wanted.is_empty() {
        return Release {
            tag: wanted.to_string(),
            tarball: format!("{archive}tags/{wanted}.tar.gz"),
        };
    }

    let latest = fetch(&format!(
        "https://api.github.com/repos/{PROJECT}/releases/latest"
    ));
    let tag = json_field(&latest, "tag_name");
    if !tag.is_empty() {
        let mut tarball = json_field(&latest, "tarball_url");
        if tarball.is_empty() {
            tarball = format!("{archive}tags/{tag}.tar.gz");
        }
        return Release { tag, tarball };
    }

    let tags = fetch(&format!("https://api.github.com/repos/{PROJECT}/tags"));
    let tag = json_field(&tags, "name");
    if !tag.is_empty() {
        let tarball = format!("{archive}tags/{tag}.tar.gz");
        return Release { tag, tarball };
    }

    Release {
        tag: "main".to_string(),
        tarball: format!("{archive}heads/main.tar.gz"),
    }
}

fn fetch(url: &str) -> String {
    capture("curl", &["-sS", "--max-time", "30", url])
}

fn latest_ghidra_version() -> String {
    let json = fetch("https://api.github.com/repos/NationalSecurityAgency/ghidra/tags");
    let marker = "\"name\": \"Ghidra_";
    let Some(at) = json.find(marker) else {
        return String::new();
    };
    let rest = &json[at + marker.len()..];
    match rest.find("_build") {
        Some(end) => rest[..end].to_string(),
        None => String::new(),
    }
}

// ---- the build made for this machine ------------------------------------

/// What this machine's archive is called in a release. The name is decided at
/// build time by the CMake that made the archive, and read here the same way,
/// so the two agree without anything having to be looked up.
fn platform() -> Option<&'static str> {
    let system = if cfg!(target_os = "macos") {
        "macos"
    } else if cfg!(target_os = "linux") {
        "linux"
    } else {
        return None;
    };
    let machine = if cfg!(target_arch = "aarch64") {
        "arm64"
    } else if cfg!(target_arch = "x86_64") {
        "x86_64"
    } else {
        return None;
    };
    Some(match (system, machine) {
        ("macos", "arm64") => "macos-arm64",
        ("macos", _) => "macos-x86_64",
        (_, "arm64") => "linux-arm64",
        _ => "linux-x86_64",
    })
}

/// Every file attached to a release, by the URL it is downloaded from.
fn assets(json: &str) -> Vec<String> {
    let mut found = Vec::new();
    let marker = "\"browser_download_url\"";
    let mut rest = json;
    while let Some(at) = rest.find(marker) {
        rest = &rest[at + marker.len()..];
        let Some(open) = rest.find('"') else { break };
        let after = &rest[open + 1..];
        let Some(close) = after.find('"') else { break };
        found.push(after[..close].to_string());
        rest = &after[close..];
    }
    found
}

/// The archive built for this machine, and the file saying what it hashes to.
fn prebuilt(tag: &str) -> Option<(String, Option<String>)> {
    let platform = platform()?;
    let json = if tag.is_empty() {
        fetch(&format!(
            "https://api.github.com/repos/{PROJECT}/releases/latest"
        ))
    } else {
        fetch(&format!(
            "https://api.github.com/repos/{PROJECT}/releases/tags/{tag}"
        ))
    };
    let urls = assets(&json);
    let wanted = format!("-{platform}.tar.gz");
    let archive = urls.iter().find(|url| url.ends_with(&wanted))?.clone();
    let checksum = format!("{archive}.sha256");
    let has_checksum = urls.iter().any(|url| *url == checksum);
    Some((archive, if has_checksum { Some(checksum) } else { None }))
}

/// What a file hashes to, asked of whichever tool this system has.
fn sha256_of(path: &str) -> String {
    let text = if have("shasum") {
        capture("shasum", &["-a", "256", path])
    } else if have("sha256sum") {
        capture("sha256sum", &[path])
    } else {
        String::new()
    };
    text.split_whitespace().next().unwrap_or("").to_string()
}

/// Downloads the build made for this machine and puts it in place. Returns None
/// when there is no such build, which is not a failure: it means build instead.
fn install_prebuilt(tag: &str, prefix: &Path, work: &Path, keep: bool) -> Option<i32> {
    let (archive, checksum) = prebuilt(tag)?;
    let name = archive.rsplit('/').next().unwrap_or("astral.tar.gz").to_string();

    let work_text = work.to_string_lossy().into_owned();
    if run("mkdir", &["-p", &work_text]) != 0 {
        return Some(1);
    }
    let downloaded = format!("{work_text}/{name}");
    Sink::new(Stream::Err).write(&format!("fetching {name}\n"));
    if run(
        "curl",
        &["-sSL", "--fail", "--max-time", "1800", "-o", &downloaded, &archive],
    ) != 0
    {
        Sink::new(Stream::Err).write("astral update: could not download the release\n");
        return Some(1);
    }

    // A build is only worth installing if it is the build that was published.
    if let Some(checksum) = checksum {
        let published = capture("curl", &["-sSL", "--fail", "--max-time", "60", &checksum]);
        let published = published.split_whitespace().next().unwrap_or("").to_string();
        let actual = sha256_of(&downloaded);
        if published.is_empty() || actual.is_empty() {
            Sink::new(Stream::Err)
                .write("note: the download could not be checked against its hash\n");
        } else if published != actual {
            Sink::new(Stream::Err).write(
                "astral update: the download does not match its published hash; \
                 nothing was installed\n",
            );
            return Some(1);
        }
    }

    let opened = format!("{work_text}/opened");
    if run("rm", &["-rf", &opened]) != 0 || run("mkdir", &["-p", &opened]) != 0 {
        return Some(1);
    }
    if run("tar", &["xzf", &downloaded, "-C", &opened, "--strip-components=1"]) != 0 {
        Sink::new(Stream::Err).write("astral update: the archive could not be opened\n");
        return Some(1);
    }

    let prefix_text = prefix.to_string_lossy().into_owned();
    let from = format!("{opened}/.");
    let placed = if install_tree_writable(prefix) {
        run("mkdir", &["-p", &prefix_text]);
        run("cp", &["-R", &from, &prefix_text])
    } else {
        Sink::new(Stream::Err).write(&format!(
            "\n{prefix_text} is not writable by this user; installing with sudo\n"
        ));
        run("sudo", &["mkdir", "-p", &prefix_text]);
        run("sudo", &["cp", "-R", &from, &prefix_text])
    };
    if placed != 0 {
        return Some(1);
    }

    if !keep {
        run("rm", &["-rf", &work_text]);
    }
    print(&format!("\ninstalled to {prefix_text}\n"));
    Some(0)
}

// ---- finding and fetching a source tree ---------------------------------

/// A source tree, found by looking rather than by remembering: the directory
/// this was run from, or one above it.
fn source_tree_here() -> Option<PathBuf> {
    let mut directory = PathBuf::from(".");
    for _ in 0..3 {
        if directory.join("CMakeLists.txt").exists()
            && directory.join("src/session.cc").exists()
            && directory.join("third_party/ghidra").exists()
        {
            return Some(directory);
        }
        directory = directory.join("..");
    }
    None
}

fn download(release: &Release, work: &Path) -> Option<PathBuf> {
    let work = work.to_string_lossy().into_owned();
    if run("mkdir", &["-p", &work]) != 0 {
        return None;
    }
    let tarball = format!("{work}/astral.tar.gz");
    Sink::new(Stream::Err).write(&format!("fetching {}\n", release.tag));
    if run(
        "curl",
        &[
            "-sSL",
            "--fail",
            "--max-time",
            "1800",
            "-o",
            &tarball,
            &release.tarball,
        ],
    ) != 0
    {
        Sink::new(Stream::Err)
            .write(&format!("astral update: could not download {}\n", release.tag));
        return None;
    }

    let source = format!("{work}/src");
    if run("rm", &["-rf", &source]) != 0 || run("mkdir", &["-p", &source]) != 0 {
        return None;
    }
    if run(
        "tar",
        &["xzf", &tarball, "-C", &source, "--strip-components=1"],
    ) != 0
    {
        return None;
    }
    Some(PathBuf::from(source))
}

/// Replaces the vendored decompiler with that of another Ghidra release.
fn vendor_ghidra(root: &Path, version: &str) -> i32 {
    let root = root.to_string_lossy().into_owned();
    let work = format!("{root}/build/vendor");
    let tag = format!("Ghidra_{version}_build");
    let tarball = format!("{work}/ghidra-src.tar.gz");
    let prefix = format!("ghidra-{tag}");

    if run("mkdir", &["-p", &work]) != 0 {
        return 1;
    }
    let url = format!("{GHIDRA_REPO}/archive/refs/tags/{tag}.tar.gz");
    if run(
        "curl",
        &["-sSL", "--fail", "--max-time", "1800", "-o", &tarball, &url],
    ) != 0
    {
        Sink::new(Stream::Err).write(&format!(
            "astral update: could not download Ghidra {version}\n"
        ));
        return 1;
    }

    let extracted = format!("{work}/x");
    if run("rm", &["-rf", &extracted]) != 0 || run("mkdir", &["-p", &extracted]) != 0 {
        return 1;
    }
    let decompiler = format!("{prefix}/Ghidra/Features/Decompiler/src/decompile/cpp");
    let processors = format!("{prefix}/Ghidra/Processors");
    if run(
        "tar",
        &[
            "xzf",
            &tarball,
            "-C",
            &extracted,
            "--strip-components=1",
            &decompiler,
            &processors,
        ],
    ) != 0
    {
        return 1;
    }

    let source = format!("{extracted}/Ghidra");
    let destination = format!("{root}/third_party/ghidra");
    if run(
        "rm",
        &[
            "-rf",
            &format!("{destination}/decompile"),
            &format!("{destination}/processors"),
        ],
    ) != 0
    {
        return 1;
    }
    if run(
        "mkdir",
        &[
            "-p",
            &format!("{destination}/decompile"),
            &format!("{destination}/processors"),
        ],
    ) != 0
    {
        return 1;
    }
    if run(
        "cp",
        &[
            "-R",
            &format!("{source}/Features/Decompiler/src/decompile/cpp/."),
            &format!("{destination}/decompile/"),
        ],
    ) != 0
    {
        return 1;
    }

    // Only the language data is needed; the Java side of each processor is not.
    let script = format!(
        "for p in '{source}/Processors'/*/; do n=$(basename \"$p\"); \
         if [ -d \"$p/data/languages\" ]; then mkdir -p '{destination}/processors'/\"$n\"/data && \
         cp -R \"$p/data/languages\" '{destination}/processors'/\"$n\"/data/; fi; done"
    );
    if run("sh", &["-c", &script]) != 0 {
        return 1;
    }

    if std::fs::write(format!("{destination}/VERSION"), format!("{version}\n")).is_err() {
        Sink::new(Stream::Err).write("astral update: cannot write the vendored version\n");
        return 1;
    }
    run("rm", &["-rf", &work]);
    Sink::new(Stream::Err).write(&format!("vendored Ghidra {version}\n"));
    0
}

// ---- the command itself --------------------------------------------------

pub fn run_command(arguments: &[String]) -> i32 {
    let mut ghidra_version = String::new();
    let mut languages = String::new();
    let mut release_tag = String::new();
    let mut prefix: Option<PathBuf> = None;
    let mut jobs = String::new();
    let mut check = false;
    let mut local = false;
    let mut from_source = false;
    let mut keep = false;

    let mut index = 0;
    if arguments.first().map(String::as_str) == Some("install") {
        prefix = Some(PathBuf::from(SYSTEM_PREFIX));
        index = 1;
    }

    while index < arguments.len() {
        let argument = arguments[index].as_str();
        let value = |name: &str, index: &mut usize| -> Option<String> {
            if *index + 1 >= arguments.len() {
                Sink::new(Stream::Err)
                    .write(&format!("astral update: {name} needs a value\n"));
                return None;
            }
            *index += 1;
            Some(arguments[*index].clone())
        };
        match argument {
            "--check" => check = true,
            "--local" => local = true,
            "--source" | "--from-source" => from_source = true,
            "--keep" => keep = true,
            "--release" => match value("--release", &mut index) {
                Some(tag) => release_tag = tag,
                None => return 2,
            },
            "--ghidra" => match value("--ghidra", &mut index) {
                Some(version) => ghidra_version = version,
                None => return 2,
            },
            "--languages" => match value("--languages", &mut index) {
                Some(list) => languages = list,
                None => return 2,
            },
            "--prefix" => match value("--prefix", &mut index) {
                Some(directory) => prefix = Some(PathBuf::from(directory)),
                None => return 2,
            },
            "--jobs" => match value("--jobs", &mut index) {
                Some(count) => jobs = count,
                None => return 2,
            },
            "--help" | "-h" => return usage(Stream::Out),
            _ => return usage(Stream::Err),
        }
        index += 1;
    }
    let prefix = prefix.unwrap_or_else(default_prefix);

    if check {
        let release = newest_release(&release_tag);
        print(&format!("astral           {}\n", astral::version()));
        print(&format!(
            "newest release   {}\n",
            if release.tag.is_empty() {
                "(unknown)"
            } else {
                &release.tag
            }
        ));
        print(&format!(
            "vendored Ghidra  {}\n",
            astral::upstream_version()
        ));
        let ghidra = latest_ghidra_version();
        print(&format!(
            "newest Ghidra    {}\n",
            if ghidra.is_empty() {
                "(unknown)"
            } else {
                &ghidra
            }
        ));
        let version = astral::version();
        let tagged = format!("v{version}");
        if !release.tag.is_empty()
            && release.tag != version
            && release.tag != tagged
            && release.tag != "main"
        {
            print("\nrun: astral update\n");
        }
        return 0;
    }

    let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".to_string());

    // The build made for this machine, when there is one and nothing said to
    // build instead. Nothing here needs a compiler, so this is the path that
    // works on a machine with no toolchain at all.
    if !local && !from_source {
        let work = PathBuf::from(&home).join(".astral/update");
        if let Some(code) = install_prebuilt(&release_tag, &prefix, &work, keep) {
            return code;
        }
        Sink::new(Stream::Err)
            .write("no build was published for this machine; building from source\n");
    }

    // Downloading is the normal path: a machine that installed a release has no
    // tree at all, and one that does may have moved on from this copy.
    let mut work = PathBuf::new();
    let root = if local {
        match source_tree_here() {
            Some(root) => root,
            None => {
                Sink::new(Stream::Err).write(concat!(
                    "astral update: --local needs to be run inside a source tree.\n",
                    "Run without --local to fetch a release instead.\n",
                ));
                return 1;
            }
        }
    } else {
        let release = newest_release(&release_tag);
        work = PathBuf::from(&home).join(".astral/update");
        match download(&release, &work) {
            Some(root) => root,
            None => return 1,
        }
    };

    if !ghidra_version.is_empty() {
        if !local {
            Sink::new(Stream::Err).write(
                "note: vendoring Ghidra into a downloaded tree, which is \
                 discarded once installed\n",
            );
        }
        if vendor_ghidra(&root, &ghidra_version) != 0 {
            return 1;
        }
    }

    if !have("cmake") {
        Sink::new(Stream::Err).write("astral update: cmake is required to build\n");
        return 1;
    }

    let root_text = root.to_string_lossy().into_owned();
    let build_dir = format!("{root_text}/build");
    let prefix_text = prefix.to_string_lossy().into_owned();

    let mut configure = vec![
        "-S".to_string(),
        root_text,
        "-B".to_string(),
        build_dir.clone(),
        "-DCMAKE_BUILD_TYPE=Release".to_string(),
        format!("-DCMAKE_INSTALL_PREFIX={prefix_text}"),
    ];
    if !languages.is_empty() {
        configure.push(format!("-DASTRAL_LANGUAGES={languages}"));
    }
    let configure: Vec<&str> = configure.iter().map(String::as_str).collect();
    if run("cmake", &configure) != 0 {
        return 1;
    }

    let mut build = vec!["--build".to_string(), build_dir.clone(), "--parallel".to_string()];
    if !jobs.is_empty() {
        build.push(jobs);
    }
    let build: Vec<&str> = build.iter().map(String::as_str).collect();
    if run("cmake", &build) != 0 {
        return 1;
    }

    // Elevation is asked for only when the destination is genuinely closed.
    let installed = if install_tree_writable(&prefix) {
        run("cmake", &["--install", &build_dir])
    } else {
        Sink::new(Stream::Err).write(&format!(
            "\n{prefix_text} is not writable by this user; installing with sudo\n"
        ));
        run("sudo", &["cmake", "--install", &build_dir])
    };
    if installed != 0 {
        return 1;
    }

    if !local && !keep {
        run("rm", &["-rf", &work.to_string_lossy()]);
    }

    print(&format!("\ninstalled to {prefix_text}\n"));
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_attached_file_is_found() {
        let json = concat!(
            r#"{"assets":[{"name":"a","browser_download_url":"https://x/astral-2.1.0-macos-arm64.tar.gz"},"#,
            r#"{"name":"b","browser_download_url":"https://x/astral-2.1.0-macos-arm64.tar.gz.sha256"},"#,
            r#"{"name":"c","browser_download_url":"https://x/astral-2.1.0-linux-x86_64.tar.gz"}]}"#
        );
        let found = assets(json);
        assert_eq!(found.len(), 3);
        assert!(found[0].ends_with("macos-arm64.tar.gz"));
        assert!(found[2].ends_with("linux-x86_64.tar.gz"));
    }

    #[test]
    fn nothing_is_found_where_nothing_is_attached() {
        assert!(assets("{}").is_empty());
        assert!(assets("").is_empty());
    }

    #[test]
    fn this_machine_names_itself_the_way_the_archive_does() {
        // The name has to be one CMake also produces, or no release will ever
        // match the machine asking for it.
        let name = platform().expect("this is a platform releases are made for");
        assert!(
            matches!(
                name,
                "macos-arm64" | "macos-x86_64" | "linux-arm64" | "linux-x86_64"
            ),
            "unexpected platform name {name}"
        );
    }
}
