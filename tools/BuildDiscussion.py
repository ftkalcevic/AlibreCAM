# BuildDiscussion.py : writes Discussion.md, the conversation between Frank and
# Claude word for word, from Claude Code's saved transcripts of this project.
#
#   python tools\BuildDiscussion.py [transcript folder]
#
# The folder defaults to %USERPROFILE%\.claude\projects\D--Projects-alibre-AlibreCAM.
# Only the chat is kept: Frank's messages, Claude's replies, Claude's questions
# with Frank's answers, and plans with whether Frank approved them. Tool calls,
# file contents and command output are left out.

import datetime
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_FOLDER = os.path.join(os.path.expanduser('~'), '.claude', 'projects', 'D--Projects-alibre-AlibreCAM')
LOCAL = datetime.timezone(datetime.timedelta(hours=11))   # Frank's time (AEDT)


def local_time(stamp):
    when = datetime.datetime.fromisoformat(stamp.replace('Z', '+00:00'))
    return when.astimezone(LOCAL)


def clean(text):
    text = re.sub(r'<system-reminder>.*?</system-reminder>', '', text, flags=re.S)
    text = re.sub(r'</?pasted_content[^>]*>', '', text)
    text = re.sub(r'<artifact-content-authored-by-others/>', '', text)
    return text.strip()


def quote(text):
    return '\n'.join(('> ' + line) if line else '>' for line in text.split('\n'))


def result_text(block):
    content = block.get('content')
    if isinstance(content, str):
        return content
    return '\n'.join(part.get('text', '') for part in content or [] if isinstance(part, dict))


def read_entries(folder):
    entries = {}
    files = sorted(glob.glob(os.path.join(folder, '*.jsonl')), key=os.path.getsize, reverse=True)
    for rank, path in enumerate(files):
        for line in open(path, encoding='utf-8'):
            try:
                entry = json.loads(line)
            except ValueError:
                continue
            if entry.get('type') not in ('user', 'assistant') or entry.get('isSidechain'):
                continue
            if not entry.get('uuid') or not entry.get('timestamp'):
                continue
            if entry['uuid'] not in entries:
                entry['_side'] = rank > 0      # only in a smaller, forked transcript
                entries[entry['uuid']] = entry
    return sorted(entries.values(), key=lambda e: e['timestamp'])


def build(folder):
    tool_uses = {}          # id -> (name, input)
    done_results = set()
    items = []              # (time, speaker, text, side)

    for entry in read_entries(folder):
        message = entry.get('message') or {}
        content = message.get('content')
        when = local_time(entry['timestamp'])
        side = entry['_side']

        if entry['type'] == 'user':
            if entry.get('isMeta') or entry.get('isCompactSummary'):
                continue
            if isinstance(content, str):
                content = [{'type': 'text', 'text': content}]
            parts = []
            for block in content or []:
                if not isinstance(block, dict):
                    continue
                kind = block.get('type')
                if kind == 'text':
                    text = clean(block.get('text', ''))
                    if text.startswith('[Request interrupted'):
                        items.append((when, 'note', 'Frank stopped Claude here.', side))
                    elif text and not text.startswith('<command-') and not text.startswith('Caveat:'):
                        parts.append(text)
                elif kind == 'image':
                    parts.append('*[picture]*')
                elif kind == 'tool_result':
                    use_id = block.get('tool_use_id')
                    if use_id not in tool_uses or use_id in done_results:
                        continue
                    done_results.add(use_id)
                    name, _ = tool_uses[use_id]
                    text = result_text(block)
                    if name == 'AskUserQuestion':
                        answers = re.findall(r'"((?:[^"\\]|\\.)*?)"="((?:[^"\\]|\\.)*?)"', text, flags=re.S)
                        if answers:
                            lines = ['Answers:'] + ['- ' + q.strip() + '\n  **' + a.strip() + '**' for q, a in answers]
                            items.append((when, 'frank', '\n'.join(lines), side))
                        elif "doesn't want to proceed" in text:
                            items.append((when, 'note', 'Frank closed the questions without answering.', side))
                    elif name == 'ExitPlanMode':
                        if text.startswith('User has approved'):
                            items.append((when, 'note', 'Frank approved the plan.', side))
                        else:
                            said = re.search(r'the user said:\s*(.*)', text, flags=re.S)
                            note = 'Frank turned the plan down.'
                            items.append((when, 'note', note, side))
                            if said:
                                items.append((when, 'frank', clean(said.group(1)), side))
            if parts:
                items.append((when, 'frank', '\n\n'.join(parts), side))
        else:
            for block in content or []:
                if not isinstance(block, dict):
                    continue
                kind = block.get('type')
                if kind == 'text':
                    text = clean(block.get('text', ''))
                    if text and text != 'No response requested.':
                        items.append((when, 'claude', text, side))
                elif kind == 'tool_use':
                    name = block.get('name')
                    data = block.get('input') or {}
                    tool_uses[block.get('id')] = (name, data)
                    if name == 'AskUserQuestion':
                        lines = ['Questions:']
                        for question in data.get('questions', []):
                            lines.append('- ' + question.get('question', ''))
                            for option in question.get('options', []):
                                lines.append('  - ' + option.get('label', '') +
                                             (': ' + option['description'] if option.get('description') else ''))
                        items.append((when, 'claude', '\n'.join(lines), side))
                    elif name == 'ExitPlanMode' and data.get('plan'):
                        items.append((when, 'claude', 'Plan:\n\n' + data['plan'].strip(), side))

    return items


def write(items, path):
    out = ['# Discussion', '',
           'The conversation between Frank and Claude, word for word, built from Claude Code\'s transcripts by '
           '`tools\\BuildDiscussion.py`. Times are Frank\'s (AEDT). Only the chat is here: Claude\'s tool calls, '
           'file edits and command output are left out. Pictures Frank pasted show as *[picture]*.',
           '',
           'Decisions are also recorded in the spec (`docs\\SPEC.md`) and `CLAUDE.md`.', '']
    main = [i for i in items if not i[3]]
    side = [i for i in items if i[3]]

    def emit(group):
        day = None
        last = None
        for when, speaker, text, _ in group:
            if when.date() != day:
                day = when.date()
                out.extend(['## ' + day.isoformat(), ''])
                last = None
            if speaker == 'note':
                out.extend(['*(' + text + ')*', ''])
                last = None
                continue
            if speaker != last:
                heading = ('### \U0001F9D1 Frank · ' + when.strftime('%H:%M')) if speaker == 'frank' \
                    else '### \U0001F916 Claude'
                out.extend([heading, ''])
                last = speaker
            out.extend([quote(text), ''])

    emit(main)
    if side:
        out.extend(['---', '', '# Side conversation', '',
                    'A forked copy of the session, kept apart from the main one.', ''])
        emit(side)
    with open(path, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('\n'.join(out).rstrip('\n') + '\n')


if __name__ == '__main__':
    folder = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_FOLDER
    items = build(folder)
    write(items, os.path.join(ROOT, 'Discussion.md'))
    print(len(items), 'entries written')
