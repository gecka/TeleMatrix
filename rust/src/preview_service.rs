// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex};

use tokio::sync::RwLock;

use crate::types::UrlPreview;

#[derive(Clone)]
pub(crate) struct PreviewService {
    /// URL -> UrlPreview cache. Only populated on successful fetches (Some) or
    /// confirmed "no preview" server responses (None with og: fields absent).
    /// Transient transport failures are not stored here; those retry next time.
    pub(crate) cache: Arc<RwLock<HashMap<String, Option<UrlPreview>>>>,
    /// URLs currently being fetched in the background. Used to deduplicate
    /// concurrent fetch_url_previews_and_notify calls for the same URL.
    pub(crate) inflight: Arc<RwLock<HashSet<String>>>,
    /// Persistent backing store for preview cache.
    pub(crate) store: Arc<Mutex<Option<crate::preview_store::PreviewStore>>>,
}

impl PreviewService {
    pub(crate) fn new(
        cache: Arc<RwLock<HashMap<String, Option<UrlPreview>>>>,
        store: Arc<Mutex<Option<crate::preview_store::PreviewStore>>>,
    ) -> Self {
        Self {
            cache,
            inflight: Arc::new(RwLock::new(HashSet::new())),
            store,
        }
    }
}
