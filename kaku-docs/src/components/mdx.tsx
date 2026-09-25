import { Accordion, Accordions } from "fumadocs-ui/components/accordion";
import { File, Files, Folder } from "fumadocs-ui/components/files";
import { GithubInfo } from "fumadocs-ui/components/github-info";
import {
  ImageZoom,
  type ImageZoomProps,
} from "fumadocs-ui/components/image-zoom";
import { Step, Steps } from "fumadocs-ui/components/steps";
import { Tab, Tabs } from "fumadocs-ui/components/tabs";
import { TypeTable } from "fumadocs-ui/components/type-table";
import defaultMdxComponents from "fumadocs-ui/mdx";
import type { MDXComponents } from "mdx/types";
import { Chip } from "./chip";

export function getMDXComponents(components?: MDXComponents) {
  return {
    ...defaultMdxComponents,
    // every Markdown image opens full-size on click
    img: (props) => <ImageZoom {...(props as ImageZoomProps)} />,
    // everything Fumadocs Studio's insert menu offers beyond the defaults
    Accordion,
    Accordions,
    File,
    Files,
    Folder,
    GithubInfo,
    Step,
    Steps,
    Tab,
    Tabs,
    TypeTable,
    Chip,
    ...components,
  } satisfies MDXComponents;
}

export const useMDXComponents = getMDXComponents;

declare global {
  type MDXProvidedComponents = ReturnType<typeof getMDXComponents>;
}
