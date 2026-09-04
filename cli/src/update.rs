//! Updating Astral.
//!
//! Most people who run this installed a release and have no source tree, so the
//! default is to fetch one: the newest release of the project, built and
//! installed here. A local tree is used only when asked for, and is found by
//! looking around rather than by a path written in at build time.

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use crate::out::{print, Sink, Stream};
use crate::paths::{default_prefix, install_tree_writable, SYSTEM_PREFIX};

const PROJECT: &str = "Hexadecimall/Astral";
const GHIDRA_REPO: &str = "https://github.com/NationalSecurityAgency/ghidra";

const HELP: &str = concat!(
    "usage: astral update [install] [options]\n",
    "\n",
    "Downloads the newest release of Astral, builds it, and installs it.\n",
    "\n",
    "  install              install into /usr/local rather than over the\n",
    "                       copy that is running\n",
    "      --check          compare what is installed against the newest\n",
    "      --release <tag>  a particular release rather than the newest\n",
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
        let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".to_string());
        work = PathBuf::from(home).join(".astral/update");
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
