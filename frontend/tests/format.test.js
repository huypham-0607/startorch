// Unit tests for the page's pure formatting logic. No dependencies:
//     node --test frontend/tests/

import assert from "node:assert/strict";
import { test } from "node:test";

import {
    UNTITLED,
    cleanTitle,
    describeResults,
    formatAuthors,
    formatMeta,
    openAlexLookupUrl,
    openAlexUrl,
    workKey,
} from "../format.js";

test("a missing or blank title becomes [Untitled]", () => {
    for (const raw of [null, undefined, 42, "", "   ", "\n\t", "<i></i>", "<b> </b>"]) {
        assert.equal(cleanTitle(raw), UNTITLED, `for ${JSON.stringify(raw)}`);
    }
    assert.equal(UNTITLED, "[Untitled]");
});

test("a real title is kept, with markup stripped and entities decoded", () => {
    assert.equal(cleanTitle("Attention Is All You Need"), "Attention Is All You Need");
    assert.equal(cleanTitle("Growth of <i>E. coli</i> in milk"), "Growth of E. coli in milk");
    assert.equal(cleanTitle("Salt &amp; pepper &lt;noise&gt;"), "Salt & pepper <noise>");
    assert.equal(cleanTitle("  Two\n  lines  "), "Two lines");
});

test("a title containing a script tag comes out as plain text", () => {
    // The page inserts titles with textContent, so this never runs either way.
    assert.equal(cleanTitle("<script>alert(1)</script>Title"), "alert(1)Title");
});

test("authors: up to three names, then et al.", () => {
    const people = (...names) => names.map((display_name) => ({ author: { display_name } }));
    assert.equal(formatAuthors(undefined), "");
    assert.equal(formatAuthors([]), "");
    assert.equal(formatAuthors(people("Ada")), "Ada");
    assert.equal(formatAuthors(people("Ada", "Alan", "Grace")), "Ada, Alan, Grace");
    assert.equal(formatAuthors(people("Ada", "Alan", "Grace", "Edsger")), "Ada, Alan, Grace et al.");
    assert.equal(formatAuthors([{ author: null }, { author: { display_name: "Ada" } }]), "Ada");
});

test("the meta line skips missing parts", () => {
    const full = {
        authorships: [{ author: { display_name: "Ashish Vaswani" } }],
        publication_year: 2017,
        primary_location: { source: { display_name: "NeurIPS" } },
        cited_by_count: 6569,
    };
    assert.equal(formatMeta(full), "Ashish Vaswani · 2017 · NeurIPS · cited by 6,569");
    assert.equal(formatMeta({ publication_year: 2020, cited_by_count: 0 }), "2020 · cited by 0");
    assert.equal(formatMeta({ primary_location: null }), "");
});

test("OpenAlex ids are matched in either form", () => {
    assert.equal(workKey("https://openalex.org/W2626778328"), "W2626778328");
    assert.equal(workKey("W2626778328"), "W2626778328");
    assert.equal(openAlexUrl("W2626778328"), "https://openalex.org/works/W2626778328");
});

test("one OpenAlex request covers the whole page, xpac included", () => {
    const url = new URL(openAlexLookupUrl(["W1", "W2", "W3"]));
    assert.equal(url.origin + url.pathname, "https://api.openalex.org/works");
    assert.equal(url.searchParams.get("filter"), "ids.openalex:W1|W2|W3");
    assert.equal(url.searchParams.get("per_page"), "3");
    assert.equal(url.searchParams.get("include_xpac"), "true");
    assert.ok(url.searchParams.get("select").includes("display_name"));
});

test("the status line reads naturally", () => {
    assert.equal(describeResults(1, 2.345), "1 result in 2.3 ms");
    assert.equal(describeResults(20, 3.14), "20 results in 3.1 ms");
    assert.equal(describeResults(20, 175.6), "20 results in 176 ms");
});
