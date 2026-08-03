// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>

namespace TeleMatrix {

/// Base class for intro flow steps (Start, Login).
/// Provides a title, description, error label, and "Next" button.
/// Supports two layout modes: cover (welcome screen with gradient)
/// and no-cover (login screen with white background).
///
/// Built from raw Qt widgets. The parent is a plain QWidget, not IntroWidget:
/// the verification steps are also hosted outside the intro, by the in-app
/// VerificationFlow.
class IntroStep : public QWidget {
    Q_OBJECT

public:
    explicit IntroStep(QWidget *parent, bool hasCover);
    ~IntroStep() override = default;

    /// Called when this step becomes visible. Override to set focus.
    virtual void activate();

    /// Called when the "Next" button is pressed. Override to handle submission.
    virtual void submit() = 0;

    /// Text shown on the Next button. Override per step.
    virtual QString nextButtonText() const = 0;

    /// Whether this step has the blue gradient cover.
    bool hasCover() const { return _hasCover; }

    /// Show an error message below the form fields.
    void showError(const QString &text);
    void hideError();

    /// Whether the "Keys: <choice>  Change" line shows at the bottom of the
    /// stage. Per the design this is first run, sign in and create only.
    void setShowsKeysLine(bool shows);
    /// The current key-storage choice, rendered into that line.
    void setKeysLabel(const QString &label);
    /// Whether the version line shows. Off inside the Add-Account popup, which
    /// is a dialog over the running app rather than the first-run stage.
    void setShowsVersion(bool shows);
    /// Whether this step offers "Skip for now". On while signing in or creating
    /// an account, where refusing to verify has to leave a way into the app, and
    /// off in the in-app verification popup — there the app is already open
    /// behind the card, so closing it is the way out and a skip that also tells
    /// the backend "never mind" would be a worse version of that.
    void setAllowsSkip(bool allows);
    [[nodiscard]] bool allowsSkip() const { return _allowsSkip; }

signals:
    /// Emitted when the step wants to navigate forward.
    void goNext();
    /// Emitted when the step wants to navigate back.
    void goBack();
    /// The "Change" link in the keys line was clicked. Key storage is a screen
    /// reached from here, not a step in the forward flow.
    void changeKeyStorage();

protected:
    void setTitleText(const QString &text);
    void setDescriptionText(const QString &text);

    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;

    QLabel *titleLabel() const { return _title; }
    QLabel *descriptionLabel() const { return _description; }
    QLabel *errorLabel() const { return _error; }
    QPushButton *nextButton() const { return _next; }
    QPushButton *backButton() const { return _back; }

    /// Create and return the standard back-arrow button (top-left).
    /// Automatically connected to goBack(). Only call once per step.
    QPushButton *addBackButton();

    /// Compute the content column left x (centered 380px column).
    int contentLeft() const;
    /// Compute the content top y (vertical centering).
    int contentTop() const;
    /// First y a step should place its own controls at — below the heading and
    /// subtitle, with the redesign's gap.
    [[nodiscard]] int contentStartTop() const;

    /// Turn a backend error into something worth showing a user: transport
    /// failures become "couldn't reach <server>" rather than the HTTP client's
    /// text with the request URL in it, and Matrix errcodes lose the code.
    /// `homeserver` is named in the message when known.
    [[nodiscard]] QString friendlyError(
        const QString &raw,
        const QString &homeserver = QString()) const;

    /// Show or hide this step's own "Skip for now" control to match
    /// allowsSkip(). Called when that answer changes; steps without such a
    /// control need not override it.
    virtual void updateSkipVisibility() {}

    /// Re-run this step's layout. The base re-places the shared chrome only;
    /// steps that position their own controls override it so error show/hide
    /// reflows them too.
    virtual void relayout();

    /// Fixed height reserved for the error message, whether or not one is
    /// showing. Constant by design: a slot that resized would move the form.
    [[nodiscard]] int errorSlotHeight() const;

    /// Place the error label just above the form's first control (all forms
    /// show errors on top). Call from the step's layout pass with the y of
    /// the first field. `slotWidth` widens the message column past the 340px
    /// form width for steps whose messages are full sentences; 0 keeps the
    /// form width.
    void placeErrorAbove(int firstControlY, int slotWidth = 0);

    /// Vertically center the whole content block. Call at the END of a step's
    /// layout pass, once every control is positioned: it measures the actual
    /// bounds of all visible content (every direct child except the back arrow)
    /// and shifts them together so the block is centered in the window, rather
    /// than assuming a fixed content height. Clamped so a tall form never rises
    /// above `introStepTopMin`.
    void centerContentVertically();

    /// Declare that this step positions the heading and subtitle itself (the
    /// key-storage screen uses a wider, left-aligned column). Stops
    /// centerContentVertically() from resetting them to the shared position.
    void setManagesHeadings(bool manages) { _managesHeadings = manages; }

private:
    void updateLayout();
    /// Put the heading and subtitle at their absolute positions. Idempotent, so
    /// centerContentVertically() can re-establish a known baseline before it
    /// measures.
    void placeHeadings();
    /// Window height owned by the stage furniture (keys + version lines).
    [[nodiscard]] int bottomReservedHeight() const;
    void paintCover(QPainter &p);
    void paintStageFurniture(QPainter &p);

    bool _hasCover = false;

    bool _managesHeadings = false;
    bool _showsKeysLine = false;
    bool _showsVersion = true;
    bool _allowsSkip = true;
    QString _keysLabel;
    /// Hit rect for the "Change" link, recomputed on every paint (the line is
    /// centred, so it moves with the window).
    QRect _keysChangeRect;

    QLabel *_title = nullptr;
    QLabel *_description = nullptr;
    QLabel *_error = nullptr;
    QPushButton *_next = nullptr;
    QPushButton *_back = nullptr;
};

} // namespace TeleMatrix
