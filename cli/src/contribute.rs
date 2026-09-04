//! Offering the learned database back to the project.
//!
//! Astral reads the policy the repository publishes and drops anything it does
//! not permit before the network is touched. A token is never required: without
//! one the records are written out and a prefilled issue is opened in the
//! browser, where the person is already signed in.

use astral::{Contribution, ContributionPolicy, Delivery, Library};

use crate::out::{error, library_error, paint, print, tint, Sink, Stream};

const HELP: &str = concat!(
    "usage: astral contribute database [options]\n",
    "       astral ctb database [options]\n",
    "\n",
    "Offers what you have taught Astral back to the project.\n",
    "\n",
    "  --dry-run            check and report, send nothing\n",
    "\n",
    "Only what a name means travels: a fingerprint and the word you chose.\n",
    "Records mentioning a path or an address never leave the machine.\n",
);

/// Where a submission goes when nobody said otherwise.
const PROJECT: &str = "Hexadecimall/Astral";

pub fn usage(stream: Stream) -> i32 {
    Sink::new(stream).write(HELP);
    stream.code()
}

pub fn run(arguments: &[String]) -> i32 {
    // A contribution always goes to the Astral project itself; there is nowhere
    // else to send it.
    let repo = PROJECT.to_string();
    let mut dry_run = false;
    let mut asked_for_database = false;

    let mut index = 0;
    while index < arguments.len() {
        match arguments[index].as_str() {
            "database" | "db" => asked_for_database = true,
            "--dry-run" | "-n" => dry_run = true,
            // Accepted for scripts; running the command is the consent.
            "--yes" | "-y" => {}
            "--help" | "-h" => return usage(Stream::Out),
            other => {
                error(&format!("unknown option {other}"));
                return 2;
            }
        }
        index += 1;
    }
    if !asked_for_database {
        return usage(Stream::Err);
    }

    let _library = match Library::new(None) {
        Ok(library) => library,
        Err(failure) => {
            library_error(&failure);
            return 1;
        }
    };

    let policy = match ContributionPolicy::ask(&repo) {
        Ok(policy) => policy,
        Err(failure) => {
            library_error(&failure);
            return 1;
        }
    };
    if !policy.accepted() {
        print(&format!(
            "Submitting... Refused: {repo} is not taking submissions.\n"
        ));
        return 1;
    }

    print("Checking for violations... ");
    let mut contribution = match Contribution::prepare(astral::knowledge().path, &policy) {
        Ok(contribution) => contribution,
        Err(failure) => {
            print("nothing to send.\n");
            library_error(&failure);
            return 1;
        }
    };

    let withheld = contribution.withheld_kind() + contribution.withheld_private();
    if withheld == 0 {
        print(&format!("{}\n", tint(paint::GREEN, "None.")));
    } else {
        print(&format!("{withheld} withheld.\n"));
    }

    if dry_run {
        print(&format!(
            "{} records ready. Nothing sent.\n",
            contribution.records()
        ));
        let message = policy.message();
        if !message.is_empty() {
            print(&format!("{message}\n"));
        }
        return 0;
    }

    print("Submitting... ");
    let url = match contribution.send(&repo, None) {
        Ok(url) => url,
        Err(failure) => {
            print(&format!("{}\n", tint(paint::BOLD_RED, "failed.")));
            library_error(&failure);
            return 1;
        }
    };

    if contribution.delivery() == Delivery::Browser {
        // Nothing has been submitted yet, so this does not claim it has: the
        // browser holds the account, and GitHub opens the pull request itself
        // once the file is dropped in.
        print("ready in your browser.\n");
        print(&format!("{url}\n"));
        print(&format!("Upload: {}\n", contribution.file()));
    } else {
        print(&format!("{}\n", tint(paint::GREEN, "Success.")));
        print(&format!("{url}\n"));
    }
    0
}
