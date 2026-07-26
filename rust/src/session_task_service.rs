// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::future::Future;
use std::sync::{Arc, Mutex, MutexGuard};

use tokio::task::JoinHandle;
use tracing::{info, warn};

#[derive(Clone)]
pub(crate) struct SessionTaskService {
    runtime_handle: tokio::runtime::Handle,
    tasks: Arc<Mutex<SessionTasks>>,
}

impl SessionTaskService {
    pub(crate) fn new(runtime_handle: tokio::runtime::Handle) -> Self {
        Self {
            runtime_handle,
            tasks: Arc::new(Mutex::new(SessionTasks::default())),
        }
    }

    pub(crate) fn tasks(&self) -> Arc<Mutex<SessionTasks>> {
        self.tasks.clone()
    }

    pub(crate) fn start_generation(&self) -> u64 {
        let mut tasks = lock_session_tasks(&self.tasks);
        let generation = tasks.start_new_generation();
        info!("Starting Matrix session task generation {generation}");
        generation
    }

    pub(crate) fn abort_current_generation(&self) {
        let mut tasks = lock_session_tasks(&self.tasks);
        tasks.cancel_current_generation();
    }

    pub(crate) fn is_generation_current(
        session_tasks: &Arc<Mutex<SessionTasks>>,
        generation: u64,
    ) -> bool {
        let tasks = lock_session_tasks(session_tasks);
        tasks.generation() == generation
    }

    pub(crate) fn register(&self, handle: JoinHandle<()>) {
        let mut tasks = lock_session_tasks(&self.tasks);
        tasks.push(handle);
    }

    pub(crate) fn spawn<F>(&self, future: F)
    where
        F: Future<Output = ()> + Send + 'static,
    {
        let handle = self.runtime_handle.spawn(future);
        self.register(handle);
    }
}

#[derive(Default)]
pub(crate) struct SessionTasks {
    generation: u64,
    handles: Vec<JoinHandle<()>>,
}

impl SessionTasks {
    fn start_new_generation(&mut self) -> u64 {
        self.cancel_current_generation()
    }

    fn cancel_current_generation(&mut self) -> u64 {
        self.abort_all();
        self.generation = self.generation.saturating_add(1);
        self.generation
    }

    fn abort_all(&mut self) {
        for handle in self.handles.drain(..) {
            handle.abort();
        }
    }

    fn push(&mut self, handle: JoinHandle<()>) {
        self.handles.retain(|h| !h.is_finished());
        self.handles.push(handle);
    }

    fn generation(&self) -> u64 {
        self.generation
    }
}

fn lock_session_tasks(mutex: &Mutex<SessionTasks>) -> MutexGuard<'_, SessionTasks> {
    match mutex.lock() {
        Ok(guard) => guard,
        Err(poisoned) => {
            warn!("Recovering poisoned Matrix mutex: session_tasks");
            poisoned.into_inner()
        }
    }
}
