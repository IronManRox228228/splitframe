import { describe, expect, it } from 'vitest';
import { buildFtsQuery, groupHasAllTerms, queryTerms } from './search-query.ts';

describe('buildFtsQuery', () => {
  it('ORs quoted terms so single-word rows can match any of them', () => {
    expect(buildFtsQuery('Hello  world')).toBe('"hello" OR "world"');
  });

  it('strips quotes and drops empty terms and duplicates', () => {
    expect(buildFtsQuery(' say "hi" say ')).toBe('"say" OR "hi"');
    expect(buildFtsQuery('   ')).toBe('');
  });

  it('keeps punctuation inside quotes so FTS syntax characters cannot break the query', () => {
    expect(buildFtsQuery('AND OR (x)')).toBe('"and" OR "or" OR "(x)"');
  });
});

describe('groupHasAllTerms', () => {
  it('requires every term in a multi-word query', () => {
    expect(groupHasAllTerms('the launch date is friday', 'launch date')).toBe(true);
    expect(groupHasAllTerms('the launch is friday', 'launch date')).toBe(false);
  });

  it('accepts any group for a single-word query', () => {
    expect(groupHasAllTerms('anything', 'launch')).toBe(true);
    expect(queryTerms('One TWO')).toEqual(['one', 'two']);
  });
});
