// SPDX-License-Identifier: GPL-3.0-or-later
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod backup_cmds;
mod cli;
mod config;
mod dialogs;
mod dlss;
mod dlss_cmds;
mod dump;
mod dump_cmds;
mod error;
mod events;
mod fsutil;
mod gamebuild;
mod guard;
mod home_count;
mod host;
mod ini;
mod lan;
mod linkcheck;
mod linkcheck_cmds;
mod nsp;
mod launch;
mod nup;
mod online;
mod open;
mod paths;
mod play;
mod presence;
mod preflight;
mod prompts;
mod profile_cmds;
mod profiles;
mod public_ip;
mod readiness;
use pokopia_relay as relay;
mod relay_cmds;
mod repair;
mod repair_cmds;
mod rollback;
mod runtime_api;
mod savebackup;
mod saveimport;
mod saves;
mod settings;
mod settings_cmds;
mod setup_job;
mod setuplog;
mod shader_archive;
mod shaders;
mod steam;
mod steam_cmds;
mod testlogs;
mod testlogs_cmds;
mod states;
mod storage;
#[cfg(debug_assertions)]
mod testhook;
mod update;
mod update_cmds;
mod window;
mod writecheck;

use std::sync::atomic::Ordering;
use tauri::Manager;

use config::{load_config, game_dir};
use paths::{exe_dir, launcher_path};
use states::States;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|a| a == "--host") {
        std::process::exit(cli::run_headless(&args));
    }
    if args.iter().any(|a| a == "--play") {
        std::process::exit(cli::run_play(&args));
    }
    readiness::UPDATED.store(args.iter().any(|a| a == "--updated"), Ordering::SeqCst);
    // WebKitGTK's DMA-BUF renderer leaves the window blank or kills it ("Error 71 (Protocol error)") on NVIDIA's driver
    #[cfg(target_os = "linux")]
    if std::env::var_os("WEBKIT_DISABLE_DMABUF_RENDERER").is_none() && std::path::Path::new("/sys/module/nvidia").exists() {
        std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "1");
    }
    states::install(tauri::Builder::default().plugin(tauri_plugin_dialog::init()))
        // a swap cut off by closing the window would have to wait for the next start to be undone
        .on_window_event(window::on_event)
        .on_page_load(window::mark_game_mode)
        .setup(|app| {
            if let Some(window) = app.get_webview_window("main") {
                // a failed screen query only means the window keeps its default size: it must never stop the launcher from starting
                let _ = window::fit_window(&window, load_config(app.handle()).window);
            }
            // Link Play starts from its defaults on every launcher start: nothing left from an earlier session decides the next one (#239)
            if let Err(e) = settings::reset_link_play(&game_dir(app.handle())) {
                eprintln!("Link Play reset: {e}");
            }
            // what the last update left: its folder, the old launcher (still exiting after a restart), a cut-off swap
            if let Some(launcher) = launcher_path() {
                let handle = app.handle().clone();
                let st = app.updates().inner().clone();
                st.set_cleaning(true);
                std::thread::spawn(move || {
                    let (root, game) = (exe_dir(), game_dir(&handle));
                    update::clean_start(&root, &update::old_files(&launcher, &game));
                    st.set_cleaning(false);
                });
            }
            #[cfg(debug_assertions)]
            testhook::drive_page(app);
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            #[cfg(debug_assertions)]
            testhook::test_report,
            config::get_config,
            readiness::launcher_state,
            update_cmds::get_releases,
            update_cmds::install_update,
            update_cmds::list_builds,
            update_cmds::install_version,
            update_cmds::cancel_update,
            open::open_release_url,
            config::save_config,
            readiness::get_overview,
            dialogs::pick_folder,
            dump_cmds::validate_dump,
            dialogs::pick_nup,
            dialogs::pick_nsp,
            dump_cmds::import_dlc,
            dump_cmds::import_nup,
            dump_cmds::cancel_nup,
            dump_cmds::save_dump,
            settings_cmds::read_settings,
            settings_cmds::write_settings,
            settings_cmds::prompt_icon_pack,
            launch::play,
            setup_job::setup_status,
            setup_job::start_setup,
            setup_job::preflight,
            setup_job::open_setup_log,
            setup_job::cancel_setup,
            storage::shader_storage,
            storage::compact_shaders,
            storage::expand_shaders,
            storage::cancel_shader_storage,
            dlss_cmds::dlss_status,
            dlss_cmds::fetch_dlss,
            launch::game_status,
            saves::save_sources,
            saves::import_save,
            saves::open_save_dir,
            profile_cmds::profiles_list,
            profile_cmds::profile_add,
            profile_cmds::profile_rename,
            profile_cmds::profile_remove,
            profile_cmds::profile_select,
            repair_cmds::repair_plan,
            repair_cmds::repair_apply,
            repair_cmds::repair_undo,
            repair_cmds::repair_last,
            backup_cmds::backup_status,
            backup_cmds::backup_list,
            backup_cmds::backup_now,
            backup_cmds::backup_restore,
            backup_cmds::open_backup_dir,
            steam_cmds::steam_status,
            steam_cmds::steam_add,
            steam_cmds::steam_remove,
            open::open_game_file,
            open::open_game_dir,
            testlogs_cmds::testing_logs_list,
            testlogs_cmds::testing_log_read,
            testlogs_cmds::testing_logs_export,
            testlogs_cmds::testing_logs_open_dir,
            relay_cmds::relay_start,
            relay_cmds::relay_stop,
            relay_cmds::relay_status,
            relay_cmds::lan_report,
            relay_cmds::public_ip,
            relay_cmds::room_code_new,
            relay_cmds::room_code_check,
            relay_cmds::room_code_host,
            linkcheck_cmds::link_probe,
            linkcheck_cmds::setup_facts,
            linkcheck_cmds::tailscale_peers,
        ])
        .run(tauri::generate_context!())
        .expect("error while running the launcher");
}
