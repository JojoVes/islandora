<?php

/**
 * @file
 * This file contains functions to create breadcrumbs on Islandora object pages.
 */

use Drupal\Core\Link;
use Drupal\Core\Url;

use Drupal\islandora\Tuque\IslandoraFedoraRepository;

/**
 * Get an array of links to be passed to drupal_set_breadcrumb().
 *
 * This is used for generating the bread-crumbs for the view object page.
 *
 * Each link in the bread-crumbs represents a member of the given objects
 * ancestry which is identified by any of the following RELS-EXT terms
 * (isMemberOf,isMemberOfCollection,isPartOf).
 *
 * @param AbstractObject $object
 *   An object whose ancestry will be mapped to bread-crumbs.
 *
 * @see drupal_set_breadcrumb()
 *
 * @return array
 *   Array of links, starting with most distant ancestor proceeding up to but
 *   not including the given object. For use in the function
 *   drupal_set_breadcrumb().
 */
function islandora_get_breadcrumbs(AbstractObject $object) {
  $breadcrumbs = [];
  if (\Drupal::config('islandora.settings')->get('islandora_render_drupal_breadcrumbs')) {
    $backend = \Drupal::config('islandora.settings')->get('islandora_breadcrumbs_backends');
    $backends = \Drupal::moduleHandler()->invokeAll('islandora_breadcrumbs_backends');
    if ($backend === ISLANDORA_BREADCRUMB_LEGACY_BACKEND || !isset($backends[$backend])) {
      $breadcrumbs = islandora_get_breadcrumbs_legacy($object->id, $object->repository);
    }
    else {
      if (isset($backends[$backend]['file'])) {
        require_once $backends[$backend]['file'];
      }
      $breadcrumbs = call_user_func($backends[$backend]['callable'], $object);
    }
    $context = 'islandora';
    \Drupal::moduleHandler()->alter('islandora_breadcrumbs', $breadcrumbs, $context, $object);
  }
  return $breadcrumbs;
}

/**
 * Call the legacy SPARQL recursive function and return the correct breadcrumbs.
 *
 * @param string $pid
 *   The object id whose parent will be fetched for the next link.
 * @param \IslandoraFedoraRepository $repository
 *   The fedora repository.
 *
 * @return array
 *   An array of links representing the breadcrumb trail, "root" first.
 */
function islandora_get_breadcrumbs_legacy($pid, IslandoraFedoraRepository $repository) {
  $breadcrumbs = islandora_get_breadcrumbs_recursive($pid, $repository);
  array_pop($breadcrumbs);
  return $breadcrumbs;
}

/**
 * Builds an array of drupal links for use in breadcrumbs.
 *
 * @todo Make fully recursive...
 *
 * @param string $pid
 *   The object id whose parent will be fetched for the next link.
 * @param FedoraRepository $repository
 *   The fedora repository.
 * @param array $context
 *   An associative array of context for internal use when recursing. Currently
 *   only used to track a single value:
 *   - level: The number of child-parent relationships to follow. Defaults to
 *     10.
 *
 * @return array
 *   An array of links representing the breadcrumb trail, "root" first.
 */
function islandora_get_breadcrumbs_recursive($pid, FedoraRepository $repository, array &$context = NULL) {
  module_load_include('inc', 'islandora', 'includes/utilities');

  // Before executing the query, we have a base case of accessing the top-level
  // collection.
  if ($context === NULL) {
    $context['level'] = 10;
  }

  $root = \Drupal::config('islandora.settings')->get('islandora_repository_pid');
  if ($pid == $root) {
    $menu_link_manager = \Drupal::service('plugin.manager.menu.link');
    $islandora_links = $menu_link_manager->loadLinksByRoute('islandora.view_default_object');
    $islandora_link = reset($islandora_links);
    return [
      Link::createFromRoute(t('Home'), '<front>'),
      Link::createFromRoute($islandora_link->getTitle(), 'islandora.view_default_object'),
    ];
  }
  else {
    $query = <<<EOQ
    PREFIX islandora-rels-ext: <http://islandora.ca/ontology/relsext#>
    PREFIX fedora-rels-ext: <info:fedora/fedora-system:def/relations-external#>
    SELECT DISTINCT ?object ?label
    FROM <#ri>
    WHERE {
        <info:fedora/!pid> ?collection_predicate ?object;
        <fedora-model:label> ?label .
        ?object <fedora-model:state> <fedora-model:Active>
        .
        !optionals
        !filters
          }
        !order
EOQ;

    $query_optionals = (array) \Drupal::moduleHandler()->invoke('islandora_xacml_api', 'islandora_basic_collection_get_query_optionals', ['view']);
    $query_filters = (array) \Drupal::moduleHandler()->invoke('islandora_xacml_api', 'islandora_basic_collection_get_query_filters');
    $filter_map = function ($filter) {
      return "FILTER($filter)";
    };
    $object = islandora_object_load($pid);
    $breadcrumb_predicates = islandora_invoke_hook_list(ISLANDORA_BREADCRUMB_FILTER_PREDICATE_HOOK, $object->models, [$object]);
    $same_term_map = function ($predicate) {
      return "sameTerm(?collection_predicate, <$predicate>)";
    };
    $collection_predicate_filters = array_map($same_term_map, $breadcrumb_predicates);
    $query_filters[] = implode(' || ', $collection_predicate_filters);
    $query_order = "ORDER BY DESC(?label)";
    $query = strtr($query, [
      '!optionals' => !empty($query_optionals) ? ('OPTIONAL {{' . implode('} UNION {', $query_optionals) . '}}') : '',
      '!filters' => implode(' ', array_map($filter_map, $query_filters)),
      '!order' => $query_order,
    ]);
    $query = strtr($query, [
      '!pid' => $pid,
    ]);
    $results = $repository->ri->sparqlQuery($query, 'unlimited');

    if (count($results) > 0 && $context['level'] > 0) {
      $parent = $results[0]['object']['value'];
      $this_label = $results[0]['label']['value'];

      if (empty($this_label)) {
        $this_label = t('-');
      }

      $context['level']--;
      return array_merge(islandora_get_breadcrumbs_recursive($parent, $repository, $context), [
        Link::createFromRoute($this_label, 'islandora.view_object', ['object' => $pid]),
      ]);

    }
    else {
      // Add an non-link, as we don't know how to get back to the root, and
      // render the last two links and break (on the next pass).
      return array_merge(
        islandora_get_breadcrumbs_recursive($root, $repository, $context),
        [Link::fromTextAndUrl('...', Url::fromUri('internal:/'))]
      );
    }
  }
}
