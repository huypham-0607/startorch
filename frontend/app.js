/**
 * The search page.
 *
 * A search makes two requests:
 *   1. /api/search, the Startorch engine: ranked ids and BM25 scores, in a few ms.
 *   2. One OpenAlex request for every card on the page: titles, authors, venues.
 *
 * Cards appear as soon as (1) returns, in rank order, and fill in when (2) does.
 * Everything from the network is inserted with textContent, never innerHTML.
 */

import { cleanTitle, describeResults, formatMeta, openAlexLookupUrl, openAlexUrl, workKey } from "./format.js";

const RESULTS_PER_PAGE = 20;
const READY_POLL_MS = 1000;
// Checked against full-en on 2026-09-22: 9-10 of each query's top 10 are scholarly
// works. Many obvious queries surface mostly OpenAlex expansion records (datasets,
// "other"), which rank high because their title-only text is short.
const EXAMPLES = [
    "Alzheimer amyloid beta",
    "Navier-Stokes turbulence",
    "inverted index compression",
    "Byzantine fault tolerance",
];

const form = document.getElementById("search-form");
const input = document.getElementById("query");
const status = document.getElementById("status");
const results = document.getElementById("results");
const examples = document.getElementById("examples");

// Each search gets a new controller. Starting one aborts the last, so a slow
// response can never overwrite a newer search's results.
let current = null;

function setStatus(text, kind = "") {
    status.textContent = text;
    status.className = kind;
}

function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
}

function link(href, text, className) {
    const a = el("a", className, text);
    a.href = href;
    a.target = "_blank";
    a.rel = "noopener";
    return a;
}

/** Builds the card for one hit, before its details arrive. */
function placeholderCard(hit, rank) {
    const li = el("li", "result");
    const article = el("article");
    const heading = el("h3");
    heading.append(el("span", "rank", `${rank}.`));
    const title = el("span", "title", "Loading…");
    title.setAttribute("aria-busy", "true");
    heading.append(title);
    const meta = el("p", "meta");
    const footer = el("p", "links");
    footer.append(el("span", "score", `BM25 ${hit.score.toFixed(2)} · ${hit.id}`));
    article.append(heading, meta, footer);
    li.append(article);
    return { id: hit.id, li, heading, title, meta, footer };
}

function fillWork(card, work) {
    const title = link(openAlexUrl(card.id), cleanTitle(work.display_name), "title");
    card.title.replaceWith(title);
    card.meta.textContent = formatMeta(work);
    const extras = [];
    if (work.doi) extras.push(link(work.doi, "DOI"));
    if (work.open_access?.oa_url) extras.push(link(work.open_access.oa_url, "Open access"));
    card.footer.prepend(...extras);
}

function fillMissing(card, message) {
    card.li.classList.add("missing");
    card.title.removeAttribute("aria-busy");
    card.title.textContent = card.id;
    card.meta.textContent = message;
}

async function fillFromOpenAlex(cards, signal) {
    let works;
    try {
        const response = await fetch(openAlexLookupUrl(cards.map((c) => c.id)), { signal });
        if (!response.ok) throw new Error(`OpenAlex returned ${response.status}`);
        const body = await response.json();
        works = new Map(body.results.map((w) => [workKey(w.id), w]));
    } catch (error) {
        if (error.name === "AbortError") return;
        for (const card of cards) {
            fillMissing(card, "Details could not be loaded from OpenAlex. Try again in a moment.");
            card.title.replaceWith(link(openAlexUrl(card.id), card.id, "title"));
        }
        return;
    }
    for (const card of cards) {
        const work = works.get(card.id);
        if (work) fillWork(card, work);
        else fillMissing(card, "No longer in OpenAlex: deleted or merged since the June 2026 snapshot.");
    }
}

/** Polls /api/readyz, then repeats the search once the index is loaded. */
async function searchWhenReady(query, controller) {
    while (!controller.signal.aborted) {
        await new Promise((resolve) => setTimeout(resolve, READY_POLL_MS));
        try {
            const response = await fetch("/api/readyz", { signal: controller.signal });
            if (response.ok) return search(query, { push: false });
        } catch (error) {
            if (error.name === "AbortError") return;
        }
    }
}

async function search(query, { push }) {
    query = query.trim();
    if (!query) return;

    current?.abort();
    const controller = new AbortController();
    current = controller;

    input.value = query;
    document.title = `${query} · Startorch`;
    if (push) history.pushState({ q: query }, "", `?q=${encodeURIComponent(query)}`);
    results.replaceChildren();
    setStatus("Searching…");

    let body;
    try {
        const response = await fetch(`/api/search?${new URLSearchParams({ query, k: RESULTS_PER_PAGE })}`, {
            signal: controller.signal,
        });
        if (response.status === 503) {
            const { detail } = await response.json();
            if (/loading/i.test(detail)) {
                setStatus("The index is still loading. The search will run as soon as it is ready.");
                searchWhenReady(query, controller);
            } else {
                setStatus("The search index failed to load. Please try again later.", "error");
            }
            return;
        }
        if (response.status === 422) {
            setStatus("Queries must be between 1 and 512 characters.", "error");
            return;
        }
        if (!response.ok) {
            setStatus(`The search service returned an error (${response.status}).`, "error");
            return;
        }
        body = await response.json();
    } catch (error) {
        if (error.name === "AbortError") return;
        setStatus("Can't reach the search service. The demo may be offline.", "error");
        return;
    }

    if (body.hits.length === 0) {
        setStatus(`No results for “${query}”. Try fewer or broader words.`);
        return;
    }
    setStatus(describeResults(body.hits.length, body.took_ms));

    const cards = body.hits.map((hit, i) => placeholderCard(hit, i + 1));
    results.replaceChildren(...cards.map((c) => c.li));

    if (body.corpus === "openalex") {
        await fillFromOpenAlex(cards, controller.signal);
    } else {
        for (const card of cards) fillMissing(card, "MS MARCO passage: no title metadata to show.");
    }
}

function queryFromUrl() {
    return new URLSearchParams(location.search).get("q") ?? "";
}

form.addEventListener("submit", (event) => {
    event.preventDefault();
    search(input.value, { push: true });
});

for (const example of EXAMPLES) {
    const button = el("button", "secondary outline", example);
    button.type = "button";
    button.addEventListener("click", () => search(example, { push: true }));
    examples.append(button);
}

// Back and forward replay the search in the URL.
window.addEventListener("popstate", () => {
    const query = queryFromUrl();
    if (query) {
        search(query, { push: false });
    } else {
        current?.abort();
        input.value = "";
        results.replaceChildren();
        setStatus("");
    }
});

const initial = queryFromUrl();
if (initial) {
    search(initial, { push: false });
} else {
    fetch("/api/readyz")
        .then(async (response) => {
            if (response.status !== 503) return;
            const { detail } = await response.json();
            if (/loading/i.test(detail)) setStatus("The index is loading. Searches will work in a moment.");
            else setStatus("The search index failed to load. Please try again later.", "error");
        })
        .catch(() => setStatus("Can't reach the search service. The demo may be offline.", "error"));
}
