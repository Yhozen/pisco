//! Xbox Series shader containers (DXBC with XDAT / XBCH / FSSM parts) and their RDNA2 code.
//! The decoder, its tables and the container parser come from XCL (GPL-2.0-only); see each file's header.
pub mod census;
pub mod container;
pub mod rdna2;
pub mod rdna2_spec;
pub mod rdna2_xbox;
pub mod xbox;
