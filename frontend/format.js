/**
 * Pure formatting helpers for the search page: no DOM, no network.
 *
 * Kept separate from app.js so Node can unit-test them:
 *     node --test frontend/tests/
 */

export const UNTITLED = "[Untitled]";
export const OPENALEX_API = "https://api.openalex.org/works";

// Only the fields a card shows, to keep the OpenAlex response small.
const CARD_FIELDS = "id,display_name,publication_year,authorships,primary_location,cited_by_count,doi,open_access";

const ENTITIES = { amp: "&", lt: "<", gt: ">", quot: '"', "#39": "'" };

/**
 * Returns a work's title as plain text, or "[Untitled]" when it has none.
 *
 * OpenAlex titles can carry inline markup such as <i>E. coli</i>. The page sets
 * titles with textContent, so markup is never executed either way; stripping it
 * only keeps it from showing as literal tags.
 */
export function cleanTitle(raw) {
    if (typeof raw !== "string") return UNTITLED;
    const text = raw
        .replace(/<[^>]*>/g, "")
        .replace(/&(amp|lt|gt|quot|#39);/g, (_, name) => ENTITIES[name])
        .replace(/\s+/g, " ")
        .trim();
    return text || UNTITLED;
}

/** Returns up to `max` author names, then "et al."; an empty string when there are none. */
export function formatAuthors(authorships, max = 3) {
    const names = (authorships ?? []).map((a) => a?.author?.display_name).filter(Boolean);
    if (names.length > max) return `${names.slice(0, max).join(", ")} et al.`;
    return names.join(", ");
}

/** Returns the meta line under a title: authors · year · venue · citations, skipping missing parts. */
export function formatMeta(work) {
    const parts = [
        formatAuthors(work.authorships),
        work.publication_year ? String(work.publication_year) : "",
        work.primary_location?.source?.display_name ?? "",
        Number.isFinite(work.cited_by_count) ? `cited by ${work.cited_by_count.toLocaleString("en-US")}` : "",
    ];
    return parts.filter(Boolean).join(" · ");
}

/** Turns an OpenAlex id in either form ("https://openalex.org/W123" or "W123") into "W123". */
export function workKey(openAlexId) {
    return String(openAlexId).split("/").pop();
}

/** Returns the OpenAlex web page for a work. */
export function openAlexUrl(id) {
    return `https://openalex.org/works/${id}`;
}

/**
 * Returns the one OpenAlex request that fetches every card on a results page.
 *
 * include_xpac=true is required: without it OpenAlex leaves out its "expansion"
 * works, which are about 42% of the English corpus.
 */
export function openAlexLookupUrl(ids) {
    const params = new URLSearchParams({
        filter: `ids.openalex:${ids.join("|")}`,
        select: CARD_FIELDS,
        per_page: String(Math.max(ids.length, 1)),
        include_xpac: "true",
    });
    return `${OPENALEX_API}?${params}`;
}

/** Returns the status line for a finished search, e.g. "20 results in 3.1 ms". */
export function describeResults(count, tookMs) {
    const time = tookMs < 10 ? tookMs.toFixed(1) : String(Math.round(tookMs));
    return `${count} result${count === 1 ? "" : "s"} in ${time} ms`;
}
