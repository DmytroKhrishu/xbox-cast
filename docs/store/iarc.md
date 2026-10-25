# IARC age-rating questionnaire: Airtime

Partner Center → submission → Age ratings. The questionnaire is only visible inside Partner Center and IARC changes
its wording from time to time, so the answers below are by intent: match each question to the closest row. Answer
accurately; Microsoft shares the publisher display name and account email with IARC and IARC can re-rate after
release ([Age ratings](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/age-ratings)).

**Expected result:** the lowest rating everywhere (ESRB Everyone, PEGI 3, USK 0, IARC 3+, ClassInd L), no content
descriptors, no interactive elements. If the result shows "Users Interact" or "Diverse Content", an answer below was
taken the wrong way: go back and re-check Q4.

## Answers

| # | Question (by intent) | Answer | Why |
|---|---|---|---|
| 1 | Category of the product | **Utility, Productivity, Communication or Other** (not Game, not Social, not Entertainment/streaming/"Reference, News or Educational") | A tool that displays the user's own device. It has no content library and no online service. Matches the Store category Utilities + tools (`listing.md`). |
| 2 | Violence, blood, gore (realistic or fantasy) | No | The app's own UI: a name, instructions, settings. |
| 3 | Sexual content, nudity, sexual references | No | Same. |
| 4 | Users can interact or exchange content with other users / user-generated content shared online / chat | **No** | See "Local mirroring is not user interaction" below. |
| 5 | Shares the user's location with other users | No | Never reads or sends location. |
| 6 | Unrestricted internet access (web browser, search engine) | No | No browser, no web view, no internet connection at all. |
| 7 | Digital purchases / in-app purchases / random items (loot boxes) | No | Free; no add-ons; no Store commerce. |
| 8 | Gambling, simulated gambling, real-money betting | No | |
| 9 | Language (profanity, crude humour) | No | |
| 10 | Controlled substances, alcohol, tobacco | No | |
| 11 | Fear / horror / disturbing content | No | |
| 12 | Discrimination, hate | No | |
| 13 | Promotes age-restricted products/services | No | |
| 14 | Collects or shares personal information (if asked) | No | `privacy.md`: nothing collected or sent. |
| 15 | Streams or provides access to third-party/licensed content (films, TV, music) (if asked) | No | It provides no content. What appears is the screen of the user's own phone, chosen and controlled by the user on that phone. |

## Local mirroring is not "user interaction" or user-generated content
IARC's interactive element "Users Interact" is defined by ESRB as "possible exposure to unfiltered/uncensored
user-generated content, including user-to-user communications and media sharing via social media and networks"
([ESRB ratings guide, Interactive Elements](https://www.esrb.org/ratings-guide/#int_elems)). Interactive elements
"highlight interactive or online features" (same page). The questions about user-generated content target content that
other people post and that the app delivers to the user (social feeds, chat, shared levels, comments).

Airtime has none of that:
- There are no other users. The only source is a device the user holds and chose in Screen Mirroring on that device,
  on the user's own local network. Optional connection code: a device must enter a code shown on the TV first.
- Nothing is sent from Airtime to anyone, nothing is published, stored or shared online, and Airtime never fetches
  content. It is functionally a wireless display cable: the same as plugging the phone into the TV with HDMI.
- No communication between people (no chat, voice, messages, profiles, friends).

So the honest answers are No to Q4 and No to Q15. The app does not act as a "portal" to a catalogue of content (the
case where PEGI uses "Parental Guidance Recommended" and ESRB uses "Diverse Content: Discretion Advised" for streaming
apps, see [ESRB criteria for non-game apps](https://www.esrb.org/esrb-ratings-criteria-for-non%e2%80%91game-apps/)):
what the TV shows is whatever the user's own phone already shows the same user.

If a question is phrased as "Can the app display content the developer does not control (for example from the user's
own device or files)?", that one is literally Yes. Answer it Yes (it is true), and expect at most a "Diverse
Content"/parental-guidance note rather than a higher age band. Do not answer Yes to "users interact / share content with
other users" for that reason; it is a different question. Record the exact wording here after the first run.

## After the questionnaire
- Save and generate; check that every market shows a rating and none says "refused classification".
- The rating carries over to updates. Retake it (Edit) only if a feature changes the answers, e.g. a future
  AirPlay video casting feature that plays URLs from the internet would make Q6/Q15 worth re-reading.
- IARC sends the rating certificate email after publication; appeals go through the link in that email.
