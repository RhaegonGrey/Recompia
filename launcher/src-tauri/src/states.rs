// SPDX-License-Identifier: GPL-3.0-or-later
//! The launcher's managed state, in one place. Tauri answers "state not managed" at run time when a command asks for a type that was not
//! put in with `manage` (a command taking `State<SetupState>` while the app manages `Arc<SetupState>` failed every call, unseen by the
//! compiler). So each state has one handle type here; `install` manages exactly these, the `*Arg` aliases are what a command parameter
//! is, and the `States` accessors are what code holding an `AppHandle` (or a window) calls. Nothing else names a managed type.

use std::sync::Arc;
use tauri::{App, AppHandle, Builder, Manager, Runtime, State, Window};

use crate::{achievements, mods_catalog, mods_update, play, relay_cmds, setup_job, storage, update};

pub type PlayHandle = play::PlayState;
pub type SetupHandle = Arc<setup_job::SetupState>;
pub type StorageHandle = Arc<storage::StorageState>;
pub type RelayHandle = relay_cmds::RelayState;
pub type UpdateHandle = Arc<update::UpdateState>;
pub type ReleasesHandle = update::ReleaseStore;
pub type CatalogHandle = mods_catalog::CatalogStore;
pub type ModsUpdateHandle = mods_update::Session;
pub type AchievementsHandle = achievements::StatsStore;

/// Command parameter types: `play: PlayArg`, `setup: SetupArg<'_>` in an async command.
pub type PlayArg<'a> = State<'a, PlayHandle>;
pub type SetupArg<'a> = State<'a, SetupHandle>;
pub type StorageArg<'a> = State<'a, StorageHandle>;
pub type RelayArg<'a> = State<'a, RelayHandle>;
pub type UpdateArg<'a> = State<'a, UpdateHandle>;

/// Puts every managed state into the app.
pub fn install<R: Runtime>(builder: Builder<R>) -> Builder<R> {
    builder
        .manage(PlayHandle::default())
        .manage(SetupHandle::default())
        .manage(StorageHandle::default())
        .manage(RelayHandle::default())
        .manage(UpdateHandle::default())
        .manage(ReleasesHandle::default())
        .manage(CatalogHandle::default())
        .manage(ModsUpdateHandle::default())
        .manage(AchievementsHandle::default())
}

/// `app.play()`, `app.setup()` ... on an `AppHandle` or a window.
pub trait States {
    fn play(&self) -> PlayArg<'_>;
    fn setup(&self) -> SetupArg<'_>;
    fn storage(&self) -> StorageArg<'_>;
    fn relay(&self) -> RelayArg<'_>;
    fn updates(&self) -> UpdateArg<'_>;
    fn releases(&self) -> State<'_, ReleasesHandle>;
    fn mod_catalog(&self) -> State<'_, CatalogHandle>;
    fn mods_update(&self) -> State<'_, ModsUpdateHandle>;
    fn achievement_stats(&self) -> State<'_, AchievementsHandle>;
}

macro_rules! states_of {
    ($t:ty) => {
        impl<R: Runtime> States for $t {
            fn play(&self) -> PlayArg<'_> {
                self.state::<PlayHandle>()
            }
            fn setup(&self) -> SetupArg<'_> {
                self.state::<SetupHandle>()
            }
            fn storage(&self) -> StorageArg<'_> {
                self.state::<StorageHandle>()
            }
            fn relay(&self) -> RelayArg<'_> {
                self.state::<RelayHandle>()
            }
            fn updates(&self) -> UpdateArg<'_> {
                self.state::<UpdateHandle>()
            }
            fn releases(&self) -> State<'_, ReleasesHandle> {
                self.state::<ReleasesHandle>()
            }
            fn mod_catalog(&self) -> State<'_, CatalogHandle> {
                self.state::<CatalogHandle>()
            }
            fn mods_update(&self) -> State<'_, ModsUpdateHandle> {
                self.state::<ModsUpdateHandle>()
            }
            fn achievement_stats(&self) -> State<'_, AchievementsHandle> {
                self.state::<AchievementsHandle>()
            }
        }
    };
}
states_of!(App<R>);
states_of!(AppHandle<R>);
states_of!(Window<R>);
