import QtQuick 2.15

PlankTextField {
    property string rememberedUsername: ""

    // Returning from another view must not replace an artist's in-progress edit.
    function prefill(onboardingUsername) {
        if (text.trim() === "")
            text = onboardingUsername !== "" ? onboardingUsername : rememberedUsername
    }
}
