//! `chunky`: the command line over `chunky_server`. Argument parsing, file I/O and printing live here; keys,
//! images, manifests, plans and the store are the library's.
#![forbid(unsafe_code)]

use std::fs;
use std::io;
use std::path::PathBuf;
use std::process::ExitCode;

use anstream::{eprintln, println};
use anstyle::{AnsiColor, Style};
use chunky::chips::PRESETS;
use chunky::descriptor::{DESCRIPTOR_LEN, ImageDescriptor, Scheme};
use chunky::geometry::FlashGeometry;
use chunky::manifest::ChunkState;
use chunky_server::keys;
use chunky_server::{DirStore, ImageMeta, Manifest, MemStore, Policy, Release, Signer, Warning, build_image, plan};
use clap::builder::{PossibleValue, PossibleValuesParser, TypedValueParser};
use clap::{Args, CommandFactory, Parser, Subcommand};
use clap_complete::Shell;

const VERSION: &str = concat!(
    env!("CARGO_PKG_VERSION"),
    " (",
    env!("CHUNKY_GIT_SHA"),
    ", built ",
    env!("CHUNKY_BUILD_TIME"),
    ")"
);

const BOLD: Style = Style::new().bold();
const GREEN: Style = Style::new().fg_color(Some(anstyle::Color::Ansi(AnsiColor::Green)));
const YELLOW: Style = Style::new().fg_color(Some(anstyle::Color::Ansi(AnsiColor::Yellow)));
const RED: Style = Style::new().fg_color(Some(anstyle::Color::Ansi(AnsiColor::Red)));

/// Delta firmware updates for small microcontrollers: build, plan and inspect.
#[derive(Parser)]
#[command(name = "chunky", version = VERSION)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Make a release key (a 32-byte Ed25519 seed, mode 600) and print its public half
    Keygen {
        /// Key file to create; an existing file is never overwritten
        key: PathBuf,
    },
    /// Print the public key of a key file: what goes into the bootloader
    Pubkey {
        /// Key file
        key: PathBuf,
    },
    /// Build and inspect images
    #[command(subcommand)]
    Image(ImageCommand),
    /// Show what a device manifest says
    Manifest {
        /// Manifest as the device sent it
        file: PathBuf,
        #[command(flatten)]
        chip: ChipArg,
    },
    /// Plan the update of a device to a new image
    Plan {
        #[command(flatten)]
        chip: ChipArg,
        /// Image the device is running (its manifest is derived, DFU empty)
        #[arg(
            long,
            value_name = "IMAGE",
            conflicts_with = "manifest",
            required_unless_present = "manifest"
        )]
        from: Option<PathBuf>,
        /// Manifest the device sent, instead of --from
        #[arg(long, value_name = "FILE")]
        manifest: Option<PathBuf>,
        /// Image to update to
        #[arg(long, value_name = "IMAGE")]
        to: PathBuf,
        /// Write the encoded plan here
        #[arg(short, long, value_name = "FILE")]
        out: Option<PathBuf>,
        /// Program blank chunks without erasing first (default: by chip geometry)
        #[arg(long, value_name = "BOOL")]
        trust_blank: Option<bool>,
    },
    /// Keep chunks of released images in a content-addressed store
    #[command(subcommand)]
    Store(StoreCommand),
    /// List the chip presets
    Chips,
    /// Print a shell completion script
    Completions {
        /// Shell
        shell: Shell,
    },
}

#[derive(Subcommand)]
enum ImageCommand {
    /// Pad a payload to the partition and seal it with a descriptor
    Build {
        /// Firmware payload (raw binary)
        payload: PathBuf,
        /// Image to write
        #[arg(short, long, value_name = "FILE")]
        out: PathBuf,
        #[command(flatten)]
        chip: ChipArg,
        /// Release key file
        #[arg(long, conflicts_with = "unsigned", required_unless_present = "unsigned")]
        key: Option<PathBuf>,
        /// Seal without a signature, for devices built with the `unsigned` opt-out (integrity only)
        #[arg(long)]
        unsigned: bool,
        /// Product id: a device refuses another product's image
        #[arg(long)]
        product: u32,
        /// Release version
        #[arg(long)]
        version: u32,
        /// Anti-rollback counter: raise it with a security fix
        #[arg(long, default_value_t = 0)]
        counter: u32,
    },
    /// Show the descriptor of an image
    Info {
        /// Image
        image: PathBuf,
    },
}

#[derive(Subcommand)]
enum StoreCommand {
    /// Add every non-blank chunk of an image
    Add {
        /// Image
        image: PathBuf,
        /// Store directory
        #[arg(long, value_name = "DIR")]
        store: PathBuf,
        #[command(flatten)]
        chip: ChipArg,
    },
    /// List the chunks in a store
    Ls {
        /// Store directory
        #[arg(long, value_name = "DIR")]
        store: PathBuf,
    },
}

#[derive(Args)]
struct ChipArg {
    /// Flash layout of the device
    #[arg(long, value_parser = chip_parser())]
    chip: String,
}

impl ChipArg {
    fn geometry(&self) -> &'static FlashGeometry {
        PRESETS
            .iter()
            .find(|(name, _)| *name == self.chip)
            .map(|(_, geometry)| *geometry)
            .expect("clap accepted only preset names")
    }
}

fn chip_parser() -> impl TypedValueParser<Value = String> {
    PossibleValuesParser::new(
        PRESETS
            .iter()
            .map(|(name, g)| PossibleValue::new(*name).help(describe_chip(g))),
    )
}

fn describe_chip(g: &FlashGeometry) -> String {
    format!(
        "{:?}: {} chunks of {} B, erase {} B, image {} KB",
        g.family,
        g.chunks(),
        g.chunk,
        g.erase,
        g.active.len / 1024
    )
}

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

fn main() -> ExitCode {
    let cli = Cli::parse();
    match run(cli.command) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{RED}error:{RED:#} {e}");
            ExitCode::FAILURE
        }
    }
}

fn run(command: Command) -> Result<()> {
    match command {
        Command::Keygen { key } => {
            let seed = keys::generate_seed()?;
            keys::write_seed(&key, &seed)?;
            println!("{}", keys::public_hex(&seed));
            eprintln!("wrote {} (mode 600): keep it off build PCs and servers", key.display());
        }
        Command::Pubkey { key } => println!("{}", keys::public_hex(&keys::read_seed(&key)?)),
        Command::Image(ImageCommand::Build {
            payload,
            out,
            chip,
            key,
            unsigned,
            product,
            version,
            counter,
        }) => {
            let signer = match (&key, unsigned) {
                (Some(key), _) => Signer::from_seed(&keys::read_seed(key)?),
                (None, _) => {
                    eprintln!("{YELLOW}warning:{YELLOW:#} unsigned image: only for devices built with `unsigned`");
                    Signer::Unsigned
                }
            };
            let meta = ImageMeta {
                product_id: product,
                version,
                security_counter: counter,
            };
            let image = build_image(&fs::read(&payload)?, chip.geometry(), &meta, &signer)?;
            fs::write(&out, &image)?;
            println!("{} {} bytes", out.display(), image.len());
            print_descriptor(&descriptor_of(&image)?);
        }
        Command::Image(ImageCommand::Info { image }) => print_descriptor(&descriptor_of(&fs::read(image)?)?),
        Command::Manifest { file, chip } => {
            let manifest = Manifest::parse(&fs::read(file)?).map_err(|e| format!("not a manifest: {e:?}"))?;
            let geometry = chip.geometry();
            println!("{BOLD}manifest{BOLD:#}");
            let scheme = if manifest.unsigned() { "unsigned" } else { "ed25519" };
            println!("  device accepts   {scheme}");
            println!("  geometry id      {:#010x}", manifest.header.geometry_id);
            if manifest.header.geometry_id != geometry.id() {
                println!("  {RED}not the layout of --chip {}{RED:#}", chip.chip);
            }
            println!("  chunks           {}", manifest.header.chunks);
            println!("  active           {}", count_states(&manifest.active));
            println!("  dfu              {}", count_states(&manifest.dfu));
        }
        Command::Plan {
            chip,
            from,
            manifest,
            to,
            out,
            trust_blank,
        } => {
            let geometry = chip.geometry();
            let mut store = MemStore::default();
            let release = Release::add(&fs::read(&to)?, geometry, &mut store)?;
            let manifest = match (manifest, from) {
                (Some(file), _) => Manifest::parse(&fs::read(file)?).map_err(|e| format!("not a manifest: {e:?}"))?,
                (None, Some(from)) => {
                    let image = fs::read(from)?;
                    let scheme = descriptor_of(&image)?.scheme;
                    Manifest::of_running_image(geometry, scheme, &image)
                }
                (None, None) => unreachable!("clap requires --from or --manifest"),
            };
            let policy = Policy { trust_blank };
            let plan = plan(geometry, &manifest, &release, &store, &policy)?;
            for warning in &plan.warnings {
                match warning {
                    Warning::UnsignedDevice => eprintln!(
                        "{YELLOW}warning:{YELLOW:#} the device accepts unsigned images: anyone who can talk to it \
                         can replace its firmware"
                    ),
                }
            }
            let s = &plan.stats;
            println!("{BOLD}plan{BOLD:#}  {} steps", plan.steps.len());
            println!("  skip             {:>5} chunks", s.skipped);
            println!(
                "  copy             {:>5} chunks  {GREEN}(0 bytes on the wire){GREEN:#}",
                s.copied
            );
            println!(
                "  write            {:>5} chunks  ({} bytes)",
                s.written,
                s.written as usize * geometry.chunk as usize
            );
            println!("  erase            {:>5} blocks", s.erased_blocks);
            println!(
                "  on the wire      {} of {} bytes ({:.1} %)",
                s.wire_bytes,
                s.image_bytes,
                100.0 * s.wire_bytes as f64 / s.image_bytes as f64
            );
            if let Some(out) = out {
                fs::write(&out, plan.encode())?;
                println!("  wrote            {}", out.display());
            }
        }
        Command::Store(StoreCommand::Add { image, store, chip }) => {
            let mut store = DirStore::open(&store)?;
            let before = store.list()?.len();
            let release = Release::add(&fs::read(image)?, chip.geometry(), &mut store)?;
            let nonblank = release.chunks.iter().filter(|c| **c != ChunkState::Blank).count();
            let added = store.list()?.len() - before;
            println!(
                "{} chunks ({nonblank} not blank), {added} new in the store",
                release.chunks.len()
            );
        }
        Command::Store(StoreCommand::Ls { store }) => {
            let blobs = DirStore::open(&store)?.list()?;
            for (hash, len) in &blobs {
                println!("{} {len}", keys::to_hex(&hash.0));
            }
            eprintln!(
                "{} chunks, {} bytes",
                blobs.len(),
                blobs.iter().map(|(_, l)| l).sum::<u64>()
            );
        }
        Command::Chips => {
            for (name, geometry) in PRESETS {
                println!("{BOLD}{name:<14}{BOLD:#} {}", describe_chip(geometry));
            }
        }
        Command::Completions { shell } => {
            clap_complete::generate(shell, &mut Cli::command(), "chunky", &mut io::stdout());
        }
    }
    Ok(())
}

fn descriptor_of(image: &[u8]) -> Result<ImageDescriptor> {
    let raw = image
        .last_chunk::<DESCRIPTOR_LEN>()
        .ok_or("not a chunky image: shorter than a descriptor")?;
    chunky_server::image::read_descriptor_unverified(raw).map_err(|e| format!("not a chunky image: {e:?}").into())
}

fn print_descriptor(d: &ImageDescriptor) {
    let scheme = match d.scheme {
        Scheme::Ed25519 => "ed25519",
        Scheme::Unsigned => "unsigned",
    };
    println!("  seal             {scheme}");
    println!("  product          {}", d.product_id);
    println!("  version          {}", d.version);
    println!("  security counter {}", d.security_counter);
    println!("  geometry id      {:#010x}", d.geometry_id);
    println!("  sha256           {}", keys::to_hex(&d.image_sha256));
}

fn count_states(states: &[ChunkState]) -> String {
    let blank = states.iter().filter(|s| **s == ChunkState::Blank).count();
    let unreadable = states.iter().filter(|s| **s == ChunkState::Unreadable).count();
    format!(
        "{} with data, {blank} blank, {unreadable} unreadable",
        states.len() - blank - unreadable
    )
}
