// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Provider-specific rules for link preview cards.
//!
//! Applies provider-aware rendering decisions after fetching OG/Matrix preview
//! metadata: default small-media selection, `og:type` mapping, and deriving a
//! site name from the URL.

use crate::types::{PreviewType, UrlPreview};

/// Lowercase and normalize a site name to a canonical key.
///
/// Handles common variations (brand names, domain suffixes, rebranding):
/// - "Twitter", "twitter.com", "X (formerly Twitter)" -> "twitter"
/// - "Facebook", "facebook.com" -> "facebook"
/// - "YouTube", "youtube.com" -> "youtube"
/// - "Instagram", "instagram.com" -> "instagram"
/// - Default: lowercase the input, strip ".com" suffix and "www." prefix.
pub fn canonicalize_site_name(site_name: &str) -> String {
    let lower = site_name.to_lowercase();
    let trimmed = lower.trim();

    // Strip "www." prefix if present.
    let stripped = trimmed.strip_prefix("www.").unwrap_or(trimmed);

    match stripped {
        // Twitter / X variations
        "twitter" | "twitter.com" | "x" | "x.com" => "twitter".to_string(),
        s if s.contains("formerly twitter") => "twitter".to_string(),

        // Facebook variations
        "facebook" | "facebook.com" | "fb" | "fb.com" => "facebook".to_string(),

        // YouTube variations
        "youtube" | "youtube.com" | "youtu.be" => "youtube".to_string(),

        // Instagram variations
        "instagram" | "instagram.com" => "instagram".to_string(),

        // Reddit variations
        "reddit" | "reddit.com" | "old.reddit.com" => "reddit".to_string(),

        // GitHub variations
        "github" | "github.com" => "github".to_string(),

        // Default: strip trailing ".com" for a cleaner key
        other => other.strip_suffix(".com").unwrap_or(other).to_string(),
    }
}

/// Apply provider-specific rules to a preview AFTER fetching metadata.
///
/// Default small-media selection by provider:
/// - Twitter, Facebook, Instagram: force `has_large_media = true` so the
///   renderer never shows a small article thumbnail.
/// - YouTube: if an image is present, set `preview_type = Video` and
///   `has_large_media = true`.
/// - Populates `site_name_canonical` for all previews.
pub fn apply_provider_rules(preview: &mut UrlPreview) {
    // Compute canonical site name from the provided site_name or URL fallback.
    let canonical = match &preview.site_name {
        Some(name) if !name.is_empty() => canonicalize_site_name(name),
        _ => {
            // Use URL-based fallback if no site_name provided.
            let fallback = fallback_site_name(&preview.url);
            if fallback.is_empty() {
                String::new()
            } else {
                canonicalize_site_name(&fallback)
            }
        }
    };

    // Populate site_name_canonical if not already set.
    if preview.site_name_canonical.is_none() && !canonical.is_empty() {
        preview.site_name_canonical = Some(canonical.clone());
    }

    // Provider-specific rules.
    // Default small-media selection matches exact siteName strings:
    //   "Twitter" | "Facebook" => NOT article = large media.
    // Since X rebranded, siteName is "X (formerly Twitter)" which no longer
    // matches the literal "Twitter", so Twitter/X cards show as article.
    // We only keep Facebook here; Twitter/Instagram use the dimension heuristic.
    match canonical.as_str() {
        "facebook" => {
            // Facebook always shows large media (exact siteName string match).
            preview.has_large_media = true;
        }
        // YouTube: if we have an image, treat as video with large media.
        "youtube" if preview.image_url.is_some() => {
            preview.preview_type = PreviewType::Video;
            preview.has_large_media = true;
        }
        // Reddit, GitHub, and all others: no special rules (generic pipeline).
        _ => {}
    }

    // For articles that didn't get has_large_media from provider rules above:
    // if the image is wide enough, default to large media.  Telegram's server
    // sends has_large_media=true for pages with large images; since Matrix
    // doesn't provide this flag, we infer it from dimensions.
    // Threshold: image width >= 400px and landscape (wider than tall).
    if !preview.has_large_media
        && preview.image_url.is_some()
        && preview.image_width >= 400
        && preview.image_width > preview.image_height
    {
        preview.has_large_media = true;
    }
}

/// Map an OG `og:type` value to our PreviewType enum.
pub fn preview_type_from_og_type(og_type: &str) -> PreviewType {
    match og_type.to_lowercase().as_str() {
        "article" => PreviewType::Article,
        "video" | "video.other" | "video.movie" | "video.episode" | "video.tv_show" => {
            PreviewType::Video
        }
        "photo" | "image" => PreviewType::Photo,
        "profile" => PreviewType::Profile,
        "music.song" | "music.album" | "music.playlist" | "music.radio_station" => {
            PreviewType::Document
        }
        "website" => PreviewType::Article,
        _ => PreviewType::Article,
    }
}

/// Extract a human-readable site name from a URL as a fallback.
///
/// Strips scheme and path, takes the last two domain components,
/// capitalizes the second-level domain.
///
/// Example: "https://docs.example.com/page" -> "Example.com"
pub fn fallback_site_name(url: &str) -> String {
    // Strip scheme (e.g. "https://").
    let without_scheme = url.find("://").map(|i| &url[i + 3..]).unwrap_or(url);

    // Strip path (everything after first '/').
    let host = match without_scheme.find('/') {
        Some(i) if i > 0 => &without_scheme[..i],
        _ => without_scheme,
    };

    // Strip port if present.
    let host = match host.rfind(':') {
        Some(i) => &host[..i],
        None => host,
    };

    // Split by '.' and take the last two components.
    let components: Vec<&str> = host.split('.').filter(|s| !s.is_empty()).collect();
    if components.len() >= 2 {
        let sld = components[components.len() - 2]; // second-level domain
        let tld = components[components.len() - 1]; // top-level domain

        // Capitalize first letter of second-level domain.
        let mut capitalized = String::with_capacity(sld.len());
        for (i, ch) in sld.chars().enumerate() {
            if i == 0 {
                for upper in ch.to_uppercase() {
                    capitalized.push(upper);
                }
            } else {
                capitalized.push(ch);
            }
        }

        format!("{}.{}", capitalized, tld)
    } else {
        String::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // -- canonicalize_site_name tests --

    #[test]
    fn test_canonicalize_twitter_variants() {
        assert_eq!(canonicalize_site_name("Twitter"), "twitter");
        assert_eq!(canonicalize_site_name("twitter.com"), "twitter");
        assert_eq!(canonicalize_site_name("X (formerly Twitter)"), "twitter");
        assert_eq!(canonicalize_site_name("x.com"), "twitter");
        assert_eq!(canonicalize_site_name("X"), "twitter");
    }

    #[test]
    fn test_canonicalize_facebook() {
        assert_eq!(canonicalize_site_name("Facebook"), "facebook");
        assert_eq!(canonicalize_site_name("facebook.com"), "facebook");
    }

    #[test]
    fn test_canonicalize_youtube() {
        assert_eq!(canonicalize_site_name("YouTube"), "youtube");
        assert_eq!(canonicalize_site_name("youtube.com"), "youtube");
    }

    #[test]
    fn test_canonicalize_instagram() {
        assert_eq!(canonicalize_site_name("Instagram"), "instagram");
        assert_eq!(canonicalize_site_name("instagram.com"), "instagram");
    }

    #[test]
    fn test_canonicalize_default_strips_com() {
        assert_eq!(canonicalize_site_name("example.com"), "example");
        assert_eq!(canonicalize_site_name("SomeWeirdSite"), "someweirdsite");
    }

    #[test]
    fn test_canonicalize_www_prefix() {
        assert_eq!(canonicalize_site_name("www.youtube.com"), "youtube");
        assert_eq!(canonicalize_site_name("www.example.com"), "example");
    }

    // -- preview_type_from_og_type tests --

    #[test]
    fn test_og_type_article() {
        assert_eq!(preview_type_from_og_type("article"), PreviewType::Article);
    }

    #[test]
    fn test_og_type_video_variants() {
        assert_eq!(preview_type_from_og_type("video"), PreviewType::Video);
        assert_eq!(preview_type_from_og_type("video.other"), PreviewType::Video);
        assert_eq!(preview_type_from_og_type("video.movie"), PreviewType::Video);
        assert_eq!(
            preview_type_from_og_type("video.episode"),
            PreviewType::Video
        );
    }

    #[test]
    fn test_og_type_photo() {
        assert_eq!(preview_type_from_og_type("photo"), PreviewType::Photo);
        assert_eq!(preview_type_from_og_type("image"), PreviewType::Photo);
    }

    #[test]
    fn test_og_type_profile() {
        assert_eq!(preview_type_from_og_type("profile"), PreviewType::Profile);
    }

    #[test]
    fn test_og_type_music() {
        assert_eq!(
            preview_type_from_og_type("music.song"),
            PreviewType::Document
        );
        assert_eq!(
            preview_type_from_og_type("music.album"),
            PreviewType::Document
        );
    }

    #[test]
    fn test_og_type_website_and_default() {
        assert_eq!(preview_type_from_og_type("website"), PreviewType::Article);
        assert_eq!(
            preview_type_from_og_type("something_unknown"),
            PreviewType::Article
        );
    }

    // -- fallback_site_name tests --

    #[test]
    fn test_fallback_basic() {
        assert_eq!(
            fallback_site_name("https://docs.example.com/page"),
            "Example.com"
        );
    }

    #[test]
    fn test_fallback_www() {
        assert_eq!(
            fallback_site_name("https://www.google.com/search?q=test"),
            "Google.com"
        );
    }

    #[test]
    fn test_fallback_no_path() {
        assert_eq!(fallback_site_name("https://github.com"), "Github.com");
    }

    #[test]
    fn test_fallback_no_scheme() {
        assert_eq!(fallback_site_name("example.org/path"), "Example.org");
    }

    #[test]
    fn test_fallback_single_component() {
        assert_eq!(fallback_site_name("localhost"), "");
    }

    #[test]
    fn test_fallback_with_port() {
        assert_eq!(
            fallback_site_name("http://example.com:8080/path"),
            "Example.com"
        );
    }

    // -- apply_provider_rules tests --

    fn make_preview(site_name: Option<&str>, url: &str) -> UrlPreview {
        UrlPreview {
            url: url.to_string(),
            site_name: site_name.map(|s| s.to_string()),
            title: Some("Test Title".to_string()),
            description: Some("Test description".to_string()),
            image_url: Some("https://example.com/image.jpg".to_string()),
            image_width: 800,
            image_height: 600,
            preview_type: PreviewType::Article,
            duration_secs: 0,
            author: None,
            has_large_media: false,
            site_name_canonical: None,
        }
    }

    #[test]
    fn test_twitter_landscape_uses_dimension_heuristic() {
        // 800x600 landscape image triggers dimension-based large media.
        let mut preview = make_preview(Some("Twitter"), "https://twitter.com/user/status/123");
        apply_provider_rules(&mut preview);
        assert!(preview.has_large_media); // from dimension heuristic, not provider rule
        assert_eq!(preview.site_name_canonical.as_deref(), Some("twitter"));
    }

    #[test]
    fn test_twitter_portrait_stays_article() {
        // Portrait image: Twitter no longer forces large media (X rebranding).
        let mut preview = make_preview(
            Some("X (formerly Twitter)"),
            "https://x.com/user/status/123",
        );
        preview.image_width = 400;
        preview.image_height = 600;
        apply_provider_rules(&mut preview);
        assert!(!preview.has_large_media);
        assert_eq!(preview.site_name_canonical.as_deref(), Some("twitter"));
    }

    #[test]
    fn test_facebook_forces_large_media() {
        let mut preview = make_preview(Some("Facebook"), "https://facebook.com/post/123");
        apply_provider_rules(&mut preview);
        assert!(preview.has_large_media);
        assert_eq!(preview.site_name_canonical.as_deref(), Some("facebook"));
    }

    #[test]
    fn test_youtube_sets_video_and_large_media() {
        let mut preview = make_preview(Some("YouTube"), "https://youtube.com/watch?v=abc");
        apply_provider_rules(&mut preview);
        assert!(preview.has_large_media);
        assert_eq!(preview.preview_type, PreviewType::Video);
        assert_eq!(preview.site_name_canonical.as_deref(), Some("youtube"));
    }

    #[test]
    fn test_youtube_no_image_keeps_article() {
        let mut preview = make_preview(Some("YouTube"), "https://youtube.com/watch?v=abc");
        preview.image_url = None;
        apply_provider_rules(&mut preview);
        // Without an image, YouTube should not force Video type.
        assert!(!preview.has_large_media);
        assert_eq!(preview.preview_type, PreviewType::Article);
    }

    #[test]
    fn test_instagram_landscape_uses_dimension_heuristic() {
        // 800x600 landscape image triggers dimension-based large media.
        let mut preview = make_preview(Some("Instagram"), "https://instagram.com/p/abc");
        apply_provider_rules(&mut preview);
        assert!(preview.has_large_media); // from dimension heuristic
        assert_eq!(preview.site_name_canonical.as_deref(), Some("instagram"));
    }

    #[test]
    fn test_reddit_no_special_rules() {
        let mut preview = make_preview(Some("Reddit"), "https://reddit.com/r/test/post");
        apply_provider_rules(&mut preview);
        // Reddit has no provider-specific rules, but the 800x600 image
        // triggers the dimension-based large media heuristic.
        assert!(preview.has_large_media);
        assert_eq!(preview.preview_type, PreviewType::Article);
        assert_eq!(preview.site_name_canonical.as_deref(), Some("reddit"));
    }

    #[test]
    fn test_unknown_site_uses_url_fallback() {
        let mut preview = make_preview(None, "https://docs.example.com/article");
        apply_provider_rules(&mut preview);
        // 800x600 image triggers dimension-based large media.
        assert!(preview.has_large_media);
        assert_eq!(preview.site_name_canonical.as_deref(), Some("example"));
    }

    #[test]
    fn test_existing_canonical_not_overwritten() {
        let mut preview = make_preview(Some("Twitter"), "https://twitter.com/user/123");
        preview.site_name_canonical = Some("custom".to_string());
        apply_provider_rules(&mut preview);
        // The existing canonical should be preserved.
        assert_eq!(preview.site_name_canonical.as_deref(), Some("custom"));
        // But provider rules still apply based on the site_name.
        assert!(preview.has_large_media);
    }

    // -- dimension-based large media heuristic tests --

    #[test]
    fn test_large_landscape_image_forces_large_media() {
        let mut preview = make_preview(Some("SomeNews"), "https://somenews.com/article");
        preview.image_width = 1200;
        preview.image_height = 630;
        apply_provider_rules(&mut preview);
        assert!(preview.has_large_media);
    }

    #[test]
    fn test_small_image_stays_small_media() {
        let mut preview = make_preview(Some("SomeSite"), "https://somesite.com/page");
        preview.image_width = 100;
        preview.image_height = 100;
        apply_provider_rules(&mut preview);
        assert!(!preview.has_large_media);
    }

    #[test]
    fn test_portrait_image_stays_small_media() {
        let mut preview = make_preview(Some("SomeSite"), "https://somesite.com/page");
        preview.image_width = 400;
        preview.image_height = 600;
        apply_provider_rules(&mut preview);
        // Portrait (taller than wide) → small media.
        assert!(!preview.has_large_media);
    }

    #[test]
    fn test_no_image_stays_small_media() {
        let mut preview = make_preview(Some("SomeSite"), "https://somesite.com/page");
        preview.image_url = None;
        preview.image_width = 1200;
        preview.image_height = 630;
        apply_provider_rules(&mut preview);
        assert!(!preview.has_large_media);
    }
}
