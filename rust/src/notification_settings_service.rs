// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Account-global notification settings ("Notifications for chats"): the default
//! push rules for private chats / rooms, and user-defined keyword content rules.
//!
//! Mirrors the per-room mute mechanics in `room_action_service.rs` — raw push-rule
//! writes, never DELETE (the homeserver 413s), explicit enable after each write.
//! Category defaults are `.m.`-prefixed server rules, so we change their
//! actions+enabled (not create/replace). See docs/mute-notifications-refactoring-plan.md.

use anyhow::{anyhow, Result};
use matrix_sdk::ruma::api::client::push::{
    delete_pushrule, get_pushrules_all, set_pushrule, set_pushrule_actions, set_pushrule_enabled,
};
use matrix_sdk::ruma::push::{Action, NewPatternedPushRule, NewPushRule, RuleKind, Tweak};
use matrix_sdk::Client;

use crate::types::RoomNotificationMode;

/// Default push-rule ids per chat category (unencrypted + encrypted). We write
/// both together so E2EE and plaintext rooms behave identically.
const DM_RULE_IDS: [&str; 2] = [
    ".m.rule.room_one_to_one",
    ".m.rule.encrypted_room_one_to_one",
];
const ROOM_RULE_IDS: [&str; 2] = [".m.rule.message", ".m.rule.encrypted"];

/// Which account-global default a level applies to.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ChatCategory {
    Dm,
    Room,
}

impl ChatCategory {
    fn rule_ids(self) -> [&'static str; 2] {
        match self {
            ChatCategory::Dm => DM_RULE_IDS,
            ChatCategory::Room => ROOM_RULE_IDS,
        }
    }
    /// The rule read back as the category's canonical level.
    fn primary(self) -> &'static str {
        self.rule_ids()[0]
    }
}

/// Snapshot of the account-global "Notifications for chats" settings.
#[derive(Debug, Clone)]
pub struct NotificationSettings {
    pub dm_level: RoomNotificationMode,
    pub room_level: RoomNotificationMode,
    /// "Mentions & keywords" master toggles (each = a server default rule's enabled flag).
    pub mention_display_name: bool,
    pub mention_username: bool,
    pub mention_room: bool,
    /// Whether keyword notifications are active (any keyword rule enabled, or none exist).
    pub keywords_enabled: bool,
    pub keywords: Vec<String>,
}

/// The mention/keyword master toggles, each backed by a server default push rule
/// (keywords = the whole set of user content rules, toggled together).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NotificationToggle {
    DisplayName,
    Username,
    Room,
    Keywords,
}

/// The default push rule ids behind the display-name / username / @room toggles.
const RULE_DISPLAY_NAME: &str = ".m.rule.contains_display_name";
const RULE_USERNAME: &str = ".m.rule.contains_user_name";
const RULE_ROOMNOTIF: &str = ".m.rule.roomnotif";

pub(crate) struct NotificationSettingsService;

impl NotificationSettingsService {
    /// Read the DM/room default levels + user keyword rules from the account ruleset.
    pub(crate) async fn get(client: &Client) -> Result<NotificationSettings> {
        let resp = client
            .send(get_pushrules_all::v3::Request::new())
            .await
            .map_err(|e| anyhow!("Failed to fetch push rules: {e}"))?;

        let level_for = |primary: &str| -> RoomNotificationMode {
            resp.global
                .underride
                .iter()
                .find(|r| r.rule_id == primary)
                .map(|r| {
                    if !r.enabled {
                        RoomNotificationMode::MentionsOnly
                    } else {
                        level_from_actions(&r.actions)
                    }
                })
                .unwrap_or(RoomNotificationMode::AllMessages)
        };

        // Enabled flag of a server default rule (override / content scope).
        let override_enabled = |rule_id: &str| -> bool {
            resp.global
                .override_
                .iter()
                .find(|r| r.rule_id == rule_id)
                .map(|r| r.enabled)
                .unwrap_or(false)
        };
        let content_enabled = |rule_id: &str| -> bool {
            resp.global
                .content
                .iter()
                .find(|r| r.rule_id == rule_id)
                .map(|r| r.enabled)
                .unwrap_or(false)
        };

        // All non-builtin content-rule patterns, deduped case-insensitively. We surface
        // them regardless of `enabled` so keywords left disabled (by an older build, or
        // an external client) still show in the field and can be managed/recovered. The
        // master toggle reads "on" when any keyword rule is enabled (or none exist yet).
        let mut keywords: Vec<String> = Vec::new();
        let mut has_keyword_rule = false;
        let mut any_keyword_enabled = false;
        for r in resp.global.content.iter() {
            if is_builtin_rule_id(&r.rule_id) {
                continue;
            }
            has_keyword_rule = true;
            any_keyword_enabled |= r.enabled;
            if !keywords.iter().any(|k| k.eq_ignore_ascii_case(&r.pattern)) {
                keywords.push(r.pattern.clone());
            }
        }

        Ok(NotificationSettings {
            dm_level: level_for(ChatCategory::Dm.primary()),
            room_level: level_for(ChatCategory::Room.primary()),
            mention_display_name: override_enabled(RULE_DISPLAY_NAME),
            mention_username: content_enabled(RULE_USERNAME),
            mention_room: override_enabled(RULE_ROOMNOTIF),
            keywords_enabled: !has_keyword_rule || any_keyword_enabled,
            keywords,
        })
    }

    /// Set a category's level by rewriting the actions + enabling both its rules.
    pub(crate) async fn set_category_level(
        client: &Client,
        category: ChatCategory,
        level: RoomNotificationMode,
    ) -> Result<()> {
        let actions = actions_for_level(level);
        for rule_id in category.rule_ids() {
            let req = set_pushrule_actions::v3::Request::new(
                RuleKind::Underride,
                rule_id.to_owned(),
                actions.clone(),
            );
            client
                .send(req)
                .await
                .map_err(|e| anyhow!("Failed to set actions for {rule_id}: {e}"))?;
            // Explicitly (re-)enable — a prior mentions-only may have disabled it, and
            // an actions PUT doesn't touch the enabled flag (see room_action_service).
            let req =
                set_pushrule_enabled::v3::Request::enable(RuleKind::Underride, rule_id.to_owned());
            client
                .send(req)
                .await
                .map_err(|e| anyhow!("Failed to enable {rule_id}: {e}"))?;
        }
        Ok(())
    }

    /// Reconcile keyword content rules to `desired` via [`reconcile_keywords`]: create
    /// the desired keywords (enabled, dedup-collapsed), enable any kept keyword that was
    /// disabled, and DELETE everything removed (and duplicates) — so an empty `desired`
    /// deletes them all. Field edits never *disable* a rule (only create / enable /
    /// delete); built-in `.m.` content rules are untouched.
    pub(crate) async fn set_keywords(client: &Client, desired: Vec<String>) -> Result<()> {
        let resp = client
            .send(get_pushrules_all::v3::Request::new())
            .await
            .map_err(|e| anyhow!("Failed to fetch push rules: {e}"))?;

        // (rule_id, pattern, enabled) for the account's non-builtin content rules.
        let existing: Vec<(String, String, bool)> = resp
            .global
            .content
            .iter()
            .filter(|r| !is_builtin_rule_id(&r.rule_id))
            .map(|r| (r.rule_id.clone(), r.pattern.clone(), r.enabled))
            .collect();

        let kw_actions = vec![
            Action::Notify,
            Action::SetTweak(Tweak::Highlight(true.into())),
            Action::SetTweak(Tweak::Sound("default".into())),
        ];

        for action in reconcile_keywords(&existing, &desired) {
            match action {
                KeywordAction::Create(word) => {
                    let new_rule =
                        NewPatternedPushRule::new(word.clone(), word.clone(), kw_actions.clone());
                    let req = set_pushrule::v3::Request::new(NewPushRule::Content(new_rule));
                    client
                        .send(req)
                        .await
                        .map_err(|e| anyhow!("Failed to add keyword {word}: {e}"))?;
                    let req =
                        set_pushrule_enabled::v3::Request::enable(RuleKind::Content, word.clone());
                    client
                        .send(req)
                        .await
                        .map_err(|e| anyhow!("Failed to enable keyword {word}: {e}"))?;
                }
                KeywordAction::Enable(rule_id) => {
                    let req = set_pushrule_enabled::v3::Request::enable(
                        RuleKind::Content,
                        rule_id.clone(),
                    );
                    client
                        .send(req)
                        .await
                        .map_err(|e| anyhow!("Failed to enable keyword rule {rule_id}: {e}"))?;
                }
                KeywordAction::Delete(rule_id) => {
                    let req = delete_pushrule::v3::Request::new(RuleKind::Content, rule_id.clone());
                    client
                        .send(req)
                        .await
                        .map_err(|e| anyhow!("Failed to delete keyword rule {rule_id}: {e}"))?;
                }
            }
        }
        Ok(())
    }

    /// Toggle a mention/keyword master switch by flipping its rule's `enabled` flag.
    /// For `Keywords`, enable/disable every non-builtin content rule together.
    pub(crate) async fn set_toggle(
        client: &Client,
        toggle: NotificationToggle,
        enabled: bool,
    ) -> Result<()> {
        match toggle {
            NotificationToggle::DisplayName => {
                set_rule_enabled(client, RuleKind::Override, RULE_DISPLAY_NAME, enabled).await
            }
            NotificationToggle::Username => {
                set_rule_enabled(client, RuleKind::Content, RULE_USERNAME, enabled).await
            }
            NotificationToggle::Room => {
                set_rule_enabled(client, RuleKind::Override, RULE_ROOMNOTIF, enabled).await
            }
            NotificationToggle::Keywords => {
                let resp = client
                    .send(get_pushrules_all::v3::Request::new())
                    .await
                    .map_err(|e| anyhow!("Failed to fetch push rules: {e}"))?;
                for r in resp.global.content.iter() {
                    if !is_builtin_rule_id(&r.rule_id) {
                        set_rule_enabled(client, RuleKind::Content, &r.rule_id, enabled).await?;
                    }
                }
                Ok(())
            }
        }
    }
}

/// Enable or disable one push rule by (kind, id).
async fn set_rule_enabled(
    client: &Client,
    kind: RuleKind,
    rule_id: &str,
    enabled: bool,
) -> Result<()> {
    let req = if enabled {
        set_pushrule_enabled::v3::Request::enable(kind, rule_id.to_owned())
    } else {
        set_pushrule_enabled::v3::Request::disable(kind, rule_id.to_owned())
    };
    client
        .send(req)
        .await
        .map_err(|e| anyhow!("Failed to toggle rule {rule_id}: {e}"))?;
    Ok(())
}

/// Derive a category's level from its default rule's push actions: a rule that
/// still `Notify`s means "all messages"; anything else (empty / dont-notify) means
/// only the global mention + keyword rules fire → "mentions & keywords only".
pub(crate) fn level_from_actions(actions: &[Action]) -> RoomNotificationMode {
    if actions.iter().any(|a| matches!(a, Action::Notify)) {
        RoomNotificationMode::AllMessages
    } else {
        RoomNotificationMode::MentionsOnly
    }
}

/// The push actions to write for a category level.
pub(crate) fn actions_for_level(level: RoomNotificationMode) -> Vec<Action> {
    match level {
        RoomNotificationMode::AllMessages => {
            vec![
                Action::Notify,
                Action::SetTweak(Tweak::Sound("default".into())),
            ]
        }
        // Mentions-only (and, defensively, Mute — never valid for a category):
        // an empty action set leaves only the global mention/keyword rules.
        _ => Vec::new(),
    }
}

/// Parse the inline comma-separated keyword field: trim, drop empties, and dedupe
/// case-insensitively while preserving first-seen order and original casing.
pub(crate) fn parse_keywords(csv: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for raw in csv.split(',') {
        let word = raw.trim();
        if word.is_empty() {
            continue;
        }
        if out.iter().any(|w| w.eq_ignore_ascii_case(word)) {
            continue;
        }
        out.push(word.to_string());
    }
    out
}

/// A push-rule mutation to reconcile keyword content rules toward the desired set.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum KeywordAction {
    /// Create a new enabled content rule (rule_id = pattern = the keyword).
    Create(String),
    /// Enable an existing (disabled) content rule by its rule id — a kept keyword.
    Enable(String),
    /// Delete a content rule by its rule id — a removed keyword or a duplicate.
    Delete(String),
}

/// Reconcile existing content rules to `desired` (case-insensitive). Dup-proof and
/// idempotent: keeps exactly one enabled rule per desired keyword (enabling it if it was
/// disabled, deleting any duplicates), and DELETES every rule whose keyword is no longer
/// desired — so an empty `desired` deletes them all. Field edits never *disable* a rule
/// (only create / enable / delete). `existing` is (rule_id, pattern, enabled) for the
/// account's non-builtin content rules.
pub(crate) fn reconcile_keywords(
    existing: &[(String, String, bool)],
    desired: &[String],
) -> Vec<KeywordAction> {
    let mut actions = Vec::new();

    // 1) Each desired keyword must have exactly one enabled rule.
    for word in desired {
        let mut matches = existing
            .iter()
            .filter(|(_, pattern, _)| pattern.eq_ignore_ascii_case(word));
        if let Some((first_id, _, first_enabled)) = matches.next() {
            if !first_enabled {
                actions.push(KeywordAction::Enable(first_id.clone()));
            }
            // Collapse duplicates: delete any further rules for the same keyword.
            for (dup_id, _, _) in matches {
                actions.push(KeywordAction::Delete(dup_id.clone()));
            }
        } else {
            actions.push(KeywordAction::Create(word.clone()));
        }
    }

    // 2) Delete every rule (enabled or disabled) whose keyword is no longer desired.
    for (rule_id, pattern, _enabled) in existing {
        if !desired.iter().any(|d| d.eq_ignore_ascii_case(pattern)) {
            actions.push(KeywordAction::Delete(rule_id.clone()));
        }
    }

    actions
}

/// True for a server-managed default content rule id (e.g. `.m.rule.contains_user_name`),
/// which the keyword editor must never touch.
pub(crate) fn is_builtin_rule_id(rule_id: &str) -> bool {
    rule_id.starts_with('.')
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn level_all_when_actions_notify() {
        let actions = vec![
            Action::Notify,
            Action::SetTweak(Tweak::Sound("default".into())),
        ];
        assert_eq!(
            level_from_actions(&actions),
            RoomNotificationMode::AllMessages
        );
    }

    #[test]
    fn level_mentions_when_actions_empty() {
        assert_eq!(level_from_actions(&[]), RoomNotificationMode::MentionsOnly);
    }

    #[test]
    fn level_mentions_when_no_notify_action() {
        // A tweak with no Notify does not raise a banner → mentions-only.
        let actions = vec![Action::SetTweak(Tweak::Highlight(true.into()))];
        assert_eq!(
            level_from_actions(&actions),
            RoomNotificationMode::MentionsOnly
        );
    }

    #[test]
    fn actions_all_contains_notify() {
        let actions = actions_for_level(RoomNotificationMode::AllMessages);
        assert!(actions.iter().any(|a| matches!(a, Action::Notify)));
    }

    #[test]
    fn actions_mentions_is_empty() {
        assert!(actions_for_level(RoomNotificationMode::MentionsOnly).is_empty());
    }

    #[test]
    fn parse_keywords_trims_drops_empty_and_dedupes() {
        let got = parse_keywords("  cat , dog ,, cat ,CAT, bird ");
        assert_eq!(got, vec!["cat", "dog", "bird"]);
    }

    #[test]
    fn parse_keywords_empty_input() {
        assert!(parse_keywords("   ").is_empty());
        assert!(parse_keywords(",,,").is_empty());
    }

    fn rule(id: &str, pattern: &str, enabled: bool) -> (String, String, bool) {
        (id.to_string(), pattern.to_string(), enabled)
    }

    #[test]
    fn reconcile_empty_deletes_all() {
        // Deletes regardless of enabled state (the old bug left keywords disabled).
        let existing = vec![rule("cat", "cat", true), rule("dog", "dog", false)];
        assert_eq!(
            reconcile_keywords(&existing, &[]),
            vec![
                KeywordAction::Delete("cat".to_string()),
                KeywordAction::Delete("dog".to_string()),
            ]
        );
    }

    #[test]
    fn reconcile_recovers_kept_disabled_and_deletes_dropped() {
        // The reported real state: keywords left disabled by the old buggy save. Keeping
        // "1"/"2" re-enables them (recovery); dropping "3" deletes it. Never disables.
        let existing = vec![
            rule("1", "1", false),
            rule("2", "2", false),
            rule("3", "3", false),
        ];
        assert_eq!(
            reconcile_keywords(&existing, &["1".to_string(), "2".to_string()]),
            vec![
                KeywordAction::Enable("1".to_string()),
                KeywordAction::Enable("2".to_string()),
                KeywordAction::Delete("3".to_string()),
            ]
        );
    }

    #[test]
    fn reconcile_creates_missing_keyword() {
        assert_eq!(
            reconcile_keywords(&[], &["cat".to_string()]),
            vec![KeywordAction::Create("cat".to_string())]
        );
    }

    #[test]
    fn reconcile_reenables_disabled_match_case_insensitive() {
        let existing = vec![rule("cat", "cat", false)];
        assert_eq!(
            reconcile_keywords(&existing, &["CAT".to_string()]),
            vec![KeywordAction::Enable("cat".to_string())]
        );
    }

    #[test]
    fn reconcile_noop_when_already_enabled() {
        let existing = vec![rule("cat", "cat", true)];
        assert!(reconcile_keywords(&existing, &["cat".to_string()]).is_empty());
    }

    #[test]
    fn reconcile_collapses_duplicate_rules_for_same_keyword() {
        // Two rules with pattern "cat" (e.g. one ours, one from another client) → delete the dup.
        let existing = vec![rule("cat", "cat", true), rule("uuid-x", "cat", true)];
        assert_eq!(
            reconcile_keywords(&existing, &["cat".to_string()]),
            vec![KeywordAction::Delete("uuid-x".to_string())]
        );
    }

    #[test]
    fn reconcile_deletes_undesired_and_creates_new() {
        let existing = vec![rule("dog", "dog", true)];
        assert_eq!(
            reconcile_keywords(&existing, &["cat".to_string()]),
            vec![
                KeywordAction::Create("cat".to_string()),
                KeywordAction::Delete("dog".to_string()),
            ]
        );
    }
}
