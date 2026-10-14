//! Format 1 through chunky's hand-written parser (`chunky::plan`), the format of today. Its WRITE data is one
//! chunk, 1 KB here.
#![no_std]
#![no_main]

use chunky::plan::{PLAN_HEADER_LEN, PlanHeader, STEP_LEN, Step};
use chunky_format_size_bench::{keep, start};

const CHUNK: usize = 1024;

#[unsafe(no_mangle)]
#[unsafe(link_section = ".text.start")]
extern "C" fn _start() -> ! {
    start(|rx| {
        let Some(Ok(header)) = rx.first_chunk::<PLAN_HEADER_LEN>().map(PlanHeader::decode) else {
            return;
        };
        keep(&header);
        let mut at = PLAN_HEADER_LEN;
        while let Some(Ok(step)) = rx.get(at..).and_then(|r| r.first_chunk::<STEP_LEN>()).map(Step::decode) {
            at += STEP_LEN;
            if step.has_data() {
                let Some(data) = rx.get(at..at + CHUNK) else {
                    return;
                };
                keep(data);
                at += CHUNK;
            }
            keep(&step);
        }
    })
}
