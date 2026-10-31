import * as YAML from 'yaml';
import { parseForESLint } from 'toml-eslint-parser';
globalThis.FolderwingConfigParsers = { YAML, parseToml: parseForESLint };
