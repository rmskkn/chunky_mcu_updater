//! Power cut at every flash operation of a whole update, per flash family: stage the delta, swap, trial boot,
//! confirm or revert. Whatever the cut tears, the device boots a verified release and ends where it should.

mod common;

use chunky_sim::{F1, G0, H7Mini, Rp2350};
use common::{App, World, run_update, signer, sweep};
use proptest::prelude::*;

/// Ways each cut is torn. `CHUNKY_SWEEP_SEEDS=500 just test` digs deeper.
fn seeds() -> u64 {
    std::env::var("CHUNKY_SWEEP_SEEDS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(16)
}

macro_rules! sweeps {
    ($($name:ident: $model:ty, chunk $chunk:expr, blocks $blocks:expr;)*) => {
        $(
            mod $name {
                use super::*;

                #[test]
                fn good_release_survives_a_cut_anywhere() {
                    sweep::<$model>($chunk, $blocks, App::Good, seeds());
                }

                #[test]
                fn bad_release_reverts_despite_a_cut_anywhere() {
                    sweep::<$model>($chunk, $blocks, App::Bad, seeds());
                }
            }
        )*
    };
}

sweeps! {
    f1: F1, chunk 1024, blocks 8;
    f1_half_page_chunks: F1, chunk 512, blocks 4;
    g0: G0, chunk 2048, blocks 6;
    h7_small_chunks_in_big_sectors: H7Mini, chunk 2048, blocks 3;
    rp2350: Rp2350, chunk 4096, blocks 4;
}

fn cuts() -> impl Strategy<Value = Vec<(u64, u64)>> {
    prop::collection::vec((0u64..120, any::<u64>()), 1..5)
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(256))]

    /// Several cuts in one update, at random distances, on every family.
    #[test]
    fn repeated_cuts(cuts in cuts(), good in any::<bool>(), family in 0..4) {
        let app = if good { App::Good } else { App::Bad };
        match family {
            0 => {
                let world = World::new::<F1>(1024, 6, signer());
                run_update(&world.device::<F1>(), &world, app, &cuts);
            }
            1 => {
                let world = World::new::<G0>(2048, 4, signer());
                run_update(&world.device::<G0>(), &world, app, &cuts);
            }
            2 => {
                let world = World::new::<H7Mini>(2048, 2, signer());
                run_update(&world.device::<H7Mini>(), &world, app, &cuts);
            }
            _ => {
                let world = World::new::<Rp2350>(4096, 3, signer());
                run_update(&world.device::<Rp2350>(), &world, app, &cuts);
            }
        }
    }
}
