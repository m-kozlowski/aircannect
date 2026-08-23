# Release summaries

Release workflows always generate a grouped list of commits since the previous
version. A release can also start with a short summary written for people who
do not follow the project history.

Before creating a tag, add an optional Markdown file named after it:

```text
docs/releases/v2.0.6.md
```

The file is copied verbatim to the top of the GitHub and Gitea release notes.
The generated commit list follows under `Detailed changes`. If the file does
not exist, the workflow publishes the grouped commit list on its own.

Use plain descriptions of visible changes and their impact. Suggested
sections are:

```markdown
## Highlights

- Added ...

## Important fixes

- Fixed ...

## Compatibility

Existing configuration and stored data remain compatible.
```

Omit empty or irrelevant sections. The release title already contains the
version, so the summary does not need another version heading.
