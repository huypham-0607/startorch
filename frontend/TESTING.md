# Testing the search page

The page is hard to unit-test, so this is a manual checklist, plus two automated suites that cover the parts
that can be tested without a browser. Run the automated ones first.

Items marked *(verified)* were checked in headless Firefox on 2026-09-22, against the full-en index.

## Automated

From the repository root:

```bash
node --test frontend/tests/          # formatting: [Untitled], authors, meta line, the OpenAlex request
cd python && uv run pytest           # API, served files and their content types
```

## Setup

Start the server on full-en, the only profile whose results have OpenAlex records:

```bash
cd python
STARTORCH_PROFILE=full-en uv run fastapi dev src/startorch/api/api.py
```

Open `http://127.0.0.1:8000/`. The index takes about 15 to 20 seconds to load. Keep the browser's developer tools
open (F12): the Console shows script errors, and the Network tab shows each request.

## Checklist

Each item gives the steps, then the expected result.

### Page and search

1. **Page loads.** Open `/`.
   You see the header, the GitHub and Technical report links, the search box, and four example queries. The
   Console has no errors. *(verified)*
2. **Example query.** Click "Navier-Stokes turbulence".
   The URL becomes `/?q=Navier-Stokes%20turbulence`. The status reads "20 results in N ms". Cards numbered 1 to 20
   appear, show "Loading…" briefly, then fill in with titles. *(verified)*
3. **Card content.** Look at a few cards.
   Each has a title, then authors, year, venue and "cited by N" (missing parts are skipped). Some have DOI or Open
   access links. The right side shows the BM25 score and the id.
4. **Links.** Click a title.
   The work's page at `https://openalex.org/works/W…` opens in a new tab. *(verified)*
5. **Ranking.** Read down the list.
   Ranks go 1 to 20 in order *(verified)*, and the BM25 scores never increase.

### Missing and odd data

6. **Deleted records.** Search `attention is all you need`.
   Some cards are faded, show an id such as `W7020802439` instead of a title, and say "No longer in OpenAlex:
   deleted or merged since the June 2026 snapshot". They keep their rank. *(verified)*
7. **[Untitled].** In the Console, run:
   ```js
   const f = await import("/format.js");
   [null, "", "   ", "<i></i>", "A <i>real</i> title"].map(f.cleanTitle)
   ```
   The result is `["[Untitled]", "[Untitled]", "[Untitled]", "[Untitled]", "A real title"]`. On the page, a card
   whose title OpenAlex left empty shows "[Untitled]". *(verified by faking the OpenAlex response)*
8. **No results.** Search `the and of`, then `zzqxjvw`.
   Both say "No results for …. Try fewer or broader words." *(verified)*

### Input

9. **Empty query.** Clear the box and press Search.
   The browser asks you to fill in the field. No request appears in the Network tab.
10. **Over-long query.** The box stops at 512 characters. To go past it, run in the Console:
    `location.search = "?q=" + "a".repeat(600)`.
    The status reads "Queries must be between 1 and 512 characters." in red. *(verified)*
11. **Markup in the query.** Search `<img src=x onerror=alert(1)>`.
    No alert appears. The text shows literally in the status line. *(verified)*

### Navigation

12. **Shareable link.** After a search, copy the URL and open it in a new tab.
    The same search runs and shows the same results.
13. **Back and forward.** Search `Alzheimer amyloid beta`, then `Navier-Stokes turbulence`. Press Back, then
    Forward.
    Back shows the Alzheimer query and its results; Forward shows the Navier-Stokes ones. *(verified for Back)*
14. **Rapid searches.** Click the four example queries quickly, one after another.
    The box and results match the last one you clicked. *(verified)*

### Failures

15. **OpenAlex unreachable.** In the Network tab, block `api.openalex.org` (Firefox: right-click a request, then
    Block URL; Chrome: More tools, then Network request blocking). Search anything. Unblock afterwards.
    Cards still appear in rank order, with their ids linked to OpenAlex, and say "Details could not be loaded from
    OpenAlex." *(verified)*
16. **Backend offline.** Stop the server (Ctrl+C), then search on the open page.
    The status reads "Can't reach the search service. The demo may be offline." in red. *(verified)*
17. **Index still loading.** Restart the server, and within 15 seconds open
    `/?q=Byzantine%20fault%20tolerance`.
    The status reads "The index is still loading. The search will run as soon as it is ready." When the load
    finishes, the results appear without a reload. *(verified: results 17 s after opening)*
18. **Failed load.** Start the server with `STARTORCH_PROFILE=no-such-profile`, then open `/` and search.
    Both say "The search index failed to load. Please try again later." in red. *(verified)*
19. **MS MARCO.** Start the server without `STARTORCH_PROFILE`, and search `ocean temperature`.
    Faded cards show passage ids and "MS MARCO passage: no title metadata to show."

### Look and feel

20. **Phone width.** Turn on responsive mode in the developer tools, and set the width to 390 px.
    There is no sideways scrolling. The example queries wrap, and the cards stay readable. *(verified)*
21. **Dark mode.** Switch your system or browser to dark mode.
    The page switches to a dark theme, and all text stays readable.
22. **Keyboard.** Press Tab from the top of the page.
    Focus reaches the search box, the Search button, the example queries, and then each result link. Enter in the
    box runs the search.
